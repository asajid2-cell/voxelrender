#include "VoxelWorld.h"
#include "TerrainConstants.h"
#include "../Graphics/RHI/d3dx12.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "InfiniteChunkManager.h"
#include <spdlog/spdlog.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace VENPOD::Simulation {

namespace {
constexpr uint32_t MAT_AIR_CPU = 0;
constexpr uint32_t MAT_BEDROCK_CPU = 255;
constexpr uint32_t CPU_BRUSH_MODE_PAINT = 0;
constexpr uint32_t CPU_BRUSH_MODE_ERASE = 1;
constexpr uint32_t CPU_BRUSH_MODE_REPLACE = 2;
constexpr uint32_t CPU_BRUSH_MODE_FILL = 3;
constexpr uint32_t CPU_BRUSH_SHAPE_SPHERE = 0;
constexpr uint32_t CPU_BRUSH_SHAPE_CUBE = 1;
constexpr uint32_t CPU_BRUSH_SHAPE_CYLINDER = 2;

uint32_t PackVoxelCPU(uint32_t material, uint32_t variant, uint32_t velocity, uint32_t state) {
    return (material & 0xFFu) |
           ((variant & 0xFFu) << 8) |
           ((velocity & 0xFFu) << 16) |
           ((state & 0xFFu) << 24);
}

uint32_t GetMaterialCPU(uint32_t voxel) {
    return voxel & 0xFFu;
}

uint32_t HashVoxelVariant(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t hash = 2166136261u ^ seed;
    auto mix = [&](uint32_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    mix(static_cast<uint32_t>(x));
    mix(static_cast<uint32_t>(y));
    mix(static_cast<uint32_t>(z));
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return hash & 0xFFu;
}

int32_t FloorToInt(float value) {
    return static_cast<int32_t>(std::floor(value));
}

int32_t FloorMod(int32_t value, int32_t modulus) {
    return static_cast<int32_t>(ChunkCoord::LocalCoord(value, static_cast<uint32_t>(modulus)));
}

uint32_t RenderSlotIndexFromWorldChunk(const ChunkCoord& coord) {
    const uint32_t slotX = static_cast<uint32_t>(FloorMod(coord.x, RENDER_BUFFER_CHUNKS_X));
    const uint32_t slotY = static_cast<uint32_t>(FloorMod(coord.y, RENDER_BUFFER_CHUNKS_Y));
    const uint32_t slotZ = static_cast<uint32_t>(FloorMod(coord.z, RENDER_BUFFER_CHUNKS_Z));
    return slotX +
        slotY * RENDER_BUFFER_CHUNKS_X +
        slotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
}

glm::ivec3 RenderSlotCoordFromWorldChunk(const ChunkCoord& coord) {
    return glm::ivec3(
        FloorMod(coord.x, RENDER_BUFFER_CHUNKS_X),
        FloorMod(coord.y, RENDER_BUFFER_CHUNKS_Y),
        FloorMod(coord.z, RENDER_BUFFER_CHUNKS_Z));
}

uint32_t LocalVoxelIndex(uint32_t x, uint32_t y, uint32_t z) {
    return x + y * INFINITE_CHUNK_SIZE + z * INFINITE_CHUNK_SIZE * INFINITE_CHUNK_SIZE;
}

void LocalVoxelFromIndex(uint32_t index, uint32_t& x, uint32_t& y, uint32_t& z) {
    x = index % INFINITE_CHUNK_SIZE;
    y = (index / INFINITE_CHUNK_SIZE) % INFINITE_CHUNK_SIZE;
    z = index / (INFINITE_CHUNK_SIZE * INFINITE_CHUNK_SIZE);
}

float BrushSdf(float x, float y, float z, float cx, float cy, float cz, float radius, uint32_t shape) {
    const float px = x - cx;
    const float py = y - cy;
    const float pz = z - cz;

    if (shape == CPU_BRUSH_SHAPE_CUBE) {
        const float dx = std::abs(px) - radius;
        const float dy = std::abs(py) - radius;
        const float dz = std::abs(pz) - radius;
        const float outsideX = std::max(dx, 0.0f);
        const float outsideY = std::max(dy, 0.0f);
        const float outsideZ = std::max(dz, 0.0f);
        const float outside = std::sqrt(outsideX * outsideX + outsideY * outsideY + outsideZ * outsideZ);
        const float inside = std::min(std::max(dx, std::max(dy, dz)), 0.0f);
        return outside + inside;
    }

    if (shape == CPU_BRUSH_SHAPE_CYLINDER) {
        const float horizontal = std::sqrt(px * px + pz * pz);
        const float dx = horizontal - radius;
        const float dy = std::abs(py) - radius;
        const float outsideX = std::max(dx, 0.0f);
        const float outsideY = std::max(dy, 0.0f);
        return std::min(std::max(dx, dy), 0.0f) + std::sqrt(outsideX * outsideX + outsideY * outsideY);
    }

    return std::sqrt(px * px + py * py + pz * pz) - radius;
}
} // namespace

Result<void> VoxelWorld::Initialize(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    Graphics::DescriptorHeapManager& heapManager,
    const VoxelWorldConfig& config)
{
    if (!device) {
        return Error("Device is null");
    }

    m_config = config;
    m_heapManager = &heapManager;

    // Create ping-pong voxel buffers
    auto result = CreateVoxelBuffers(device, heapManager);
    if (!result) {
        return Error("Failed to create voxel buffers: {}", result.error());
    }

    // Create material palette texture
    result = CreateMaterialPalette(device, cmdList, heapManager);
    if (!result) {
        return Error("Failed to create material palette: {}", result.error());
    }

    m_brushRaycastCPU.posX = 0.0f;
    m_brushRaycastCPU.posY = 0.0f;
    m_brushRaycastCPU.posZ = 0.0f;
    m_brushRaycastCPU.normalPacked = 0;
    m_brushRaycastCPU.hasValidPosition = false;
    m_groundRaycastCPU.posX = 0.0f;
    m_groundRaycastCPU.posY = 40.0f;  // Default to sea level
    m_groundRaycastCPU.posZ = 0.0f;
    m_groundRaycastCPU.normalPacked = 0;
    m_groundRaycastCPU.hasValidPosition = false;

    if (m_config.enableRaycastResultBuffers) {
        // Create brush raycast result buffer (16 bytes GPU + 16 bytes CPU readback)
        result = m_brushRaycastResult.Initialize(
            device,
            16,  // 4 floats = 16 bytes (posX, posY, posZ, normalPacked)
            Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
            16,  // stride = 16 bytes (entire structure)
            "BrushRaycastResult"
        );
        if (!result) {
            return Error("Failed to create brush raycast result buffer: {}", result.error());
        }

        result = m_brushRaycastResult.CreateUAV(device, heapManager);
        if (!result) {
            return Error("Failed to create UAV for brush raycast result: {}", result.error());
        }

        // Create ground raycast result buffer (16 bytes GPU + 16 bytes CPU readback)
        result = m_groundRaycastResult.Initialize(
            device,
            16,  // 4 floats = 16 bytes (posX, posY, posZ, normalPacked)
            Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
            16,  // stride = 16 bytes (entire structure)
            "GroundRaycastResult"
        );
        if (!result) {
            return Error("Failed to create ground raycast result buffer: {}", result.error());
        }

        result = m_groundRaycastResult.CreateUAV(device, heapManager);
        if (!result) {
            return Error("Failed to create UAV for ground raycast result: {}", result.error());
        }
    } else {
        spdlog::info("VoxelWorld compatibility mode: raycast result GPU buffers disabled");
    }

    if (m_config.enableBrushEditFeedbackBuffers) {
        result = CreateBrushEditFeedbackBuffers(device, heapManager);
        if (!result) {
            return Error("Failed to create brush edit feedback buffers: {}", result.error());
        }
    } else {
        m_brushEditFeedbackAvailable = false;
        spdlog::info("VoxelWorld compatibility mode: brush edit feedback buffers disabled");
    }

    uint64_t totalMemoryMB = (GetTotalVoxels() * sizeof(uint32_t) * 2) / (1024 * 1024);
    spdlog::info("VoxelWorld initialized: {}x{}x{} grid ({} MB)",
        m_config.gridSizeX, m_config.gridSizeY, m_config.gridSizeZ, totalMemoryMB);
    if (m_config.enableRaycastResultBuffers) {
        spdlog::info("GPU brush raycasting enabled (16 bytes readback vs 32 MB!)");
        spdlog::info("GPU ground detection enabled for player collision");
    }

    // ===== INITIALIZE INFINITE CHUNK SYSTEM =====
    if (m_useInfiniteChunks) {
        m_chunkManager = std::make_unique<InfiniteChunkManager>();

        InfiniteChunkConfig chunkConfig;
        chunkConfig.worldSeed = m_config.worldSeed;
        chunkConfig.chunksPerFrame = 3;  // Match the generation allocator ring without retry churn.
        const uint32_t visibleDenseChunkCount =
            RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y * RENDER_BUFFER_CHUNKS_Z;
        chunkConfig.maxQueuedChunks =
            visibleDenseChunkCount + 768;
        chunkConfig.maxChunksQueuedPerUpdate = 256;
        // Resident+queued source chunks must cover the visible dense window, but
        // also needs enough headroom to pre-generate the next several columns.
        // A cache only slightly larger than the dense window made streaming
        // behave like a just-in-time boundary load: old source chunks were
        // trimmed before the next edge was generated, so terrain appeared only
        // after crossing into it. Keep a real prefetch margin without trying to
        // hoard the full load cube.

        // ===== SEAMLESS INFINITE STREAMING CONFIGURATION =====
        // The key to truly infinite worlds: load chunks BEFORE they're visible!
        //
        // LOAD_DISTANCE: Chunks start generating beyond the visible window
        //   - Creates a small buffer zone beyond visible area
        //   - Chunks have time to generate before player reaches them
        //   - Includes a vertical buffer so climbing/falling has ready chunks
        //
        // RENDER_DISTANCE: What's visible in the dense buffer
        // (TerrainConstants.h). This is currently a temporary dev harness size;
        // the high-scale path is sparse bricks/clipmaps rather than a giant
        // dense editable buffer.
        //
        // UNLOAD_DISTANCE: Chunks deleted at this distance
        //   - 4-chunk gap from LOAD prevents thrashing when moving back and forth
        //   - Only chunks truly far away get deleted
        //
        chunkConfig.loadDistanceHorizontal = LOAD_DISTANCE_HORIZONTAL;
        chunkConfig.unloadDistanceHorizontal = UNLOAD_DISTANCE_HORIZONTAL;
        chunkConfig.loadDistanceVerticalBelow = LOAD_DISTANCE_VERTICAL_BELOW;
        chunkConfig.loadDistanceVerticalAbove = LOAD_DISTANCE_VERTICAL_ABOVE;
        chunkConfig.unloadDistanceVerticalBelow = UNLOAD_DISTANCE_VERTICAL_BELOW;
        chunkConfig.unloadDistanceVerticalAbove = UNLOAD_DISTANCE_VERTICAL_ABOVE;

        // Faster chunk generation for smooth exploration, bounded by the
        // resident source-chunk VRAM budget above.

        auto chunkResult = m_chunkManager->Initialize(device, heapManager, chunkConfig);
        if (!chunkResult) {
            return Error("Failed to initialize infinite chunk manager: {}", chunkResult.error());
        }

        // CACHE FIX: Set callback to notify VoxelWorld when chunks are unloaded
        m_chunkManager->SetUnloadCallback([this](const ChunkCoord& coord) {
            this->OnChunkUnloaded(coord);
        });

        // Create chunk copy pipeline for UpdateActiveRegion
        result = CreateChunkCopyPipeline(device);
        if (!result) {
            return Error("Failed to create chunk copy pipeline: {}", result.error());
        }

        result = CreateEditApplyPipeline(device);
        if (!result) {
            return Error("Failed to create persistent edit apply pipeline: {}", result.error());
        }

        if (!RunEditOverlayCoordinateSelfTest()) {
            return Error("Persistent edit overlay coordinate self-test failed");
        }

        spdlog::info("Infinite chunk system enabled - load: {} chunks, render: {} chunks, unload: {} chunks (seed: {})",
            chunkConfig.loadDistanceHorizontal,
            RENDER_DISTANCE_HORIZONTAL,
            chunkConfig.unloadDistanceHorizontal,
            chunkConfig.worldSeed);

        // Initialize active region center to invalid coordinates (will update on first frame)
        m_activeRegionCenter = ChunkCoord{INT32_MAX, INT32_MAX, INT32_MAX};
        m_activeRegionNeedsUpdate = true;
    }

    return {};
}

void VoxelWorld::Shutdown() {
    m_voxelBuffers[0].Shutdown();
    m_voxelBuffers[1].Shutdown();

    // ===== SHUTDOWN INFINITE CHUNK SYSTEM (NEW) =====
    if (m_chunkManager) {
        m_chunkManager->Shutdown();
        m_chunkManager.reset();
    }

    // Cleanup chunk copy pipeline
    if (m_chunkCopyConstantBuffer) {
        if (m_chunkCopyConstantBufferMappedPtr) {
            m_chunkCopyConstantBuffer->Unmap(0, nullptr);
            m_chunkCopyConstantBufferMappedPtr = nullptr;
        }
        m_chunkCopyConstantBuffer.Reset();
    }
    m_chunkCopyPSO.Reset();
    m_chunkCopyRootSignature.Reset();
    m_chunkCopyCmdList.Reset();

    for (uint32_t i = 0; i < NUM_EDIT_UPLOAD_SLOTS; ++i) {
        if (m_editUploadBuffers[i] && m_editUploadMappedPtrs[i]) {
            m_editUploadBuffers[i]->Unmap(0, nullptr);
            m_editUploadMappedPtrs[i] = nullptr;
        }
        m_editUploadBuffers[i].Reset();
        m_editUploadCapacities[i] = 0;
    }
    m_editApplyPSO.Reset();
    m_editApplyRootSignature.Reset();

    // Cleanup ring buffer allocators and fence
    for (uint32_t i = 0; i < NUM_COPY_BUFFERS; ++i) {
        m_chunkCopyCmdAllocators[i].Reset();
    }
    if (m_chunkCopyFenceEvent) {
        CloseHandle(m_chunkCopyFenceEvent);
        m_chunkCopyFenceEvent = nullptr;
    }
    m_chunkCopyFence.Reset();

    // Free shader-visible descriptors for voxel buffers
    if (m_heapManager) {
        for (int i = 0; i < 2; i++) {
            if (m_shaderVisibleSRVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleSRVs[i]);
            }
            if (m_shaderVisibleUAVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_shaderVisibleUAVs[i]);
            }
            if (m_chunkValidMaskSRVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_chunkValidMaskSRVs[i]);
            }
            if (m_chunkValidMaskUAVs[i].IsValid()) {
                m_heapManager->FreeShaderVisibleCbvSrvUav(m_chunkValidMaskUAVs[i]);
            }
        }

        if (m_paletteSRV.IsValid()) {
            m_heapManager->FreeStagingCbvSrvUav(m_paletteSRV);
        }

        if (m_paletteShaderVisibleSRV.IsValid()) {
            m_heapManager->FreeShaderVisibleCbvSrvUav(m_paletteShaderVisibleSRV);
        }
    }

    // Cleanup brush raycast buffers
    m_chunkValidMasks[0].Shutdown();
    m_chunkValidMasks[1].Shutdown();
    m_brushRaycastResult.Shutdown();
    m_brushRaycastReadback.Reset();

    // Cleanup ground raycast buffers
    m_groundRaycastResult.Shutdown();
    m_groundRaycastReadback.Reset();

    m_brushEditEventBuffer.Shutdown();
    m_brushEditCounterBuffer.Shutdown();
    m_brushEditCounterResetUpload.Shutdown();
    for (auto& slot : m_brushEditFeedbackSlots) {
        slot.eventReadback.Shutdown();
        slot.counterReadback.Shutdown();
        slot.pending = false;
        slot.fenceValue = 0;
        slot.bufferIndex = 0;
    }
    m_activeBrushEditFeedbackSlot = -1;
    m_brushEditFeedbackAvailable = false;

    m_materialPalette.Reset();
    m_paletteUpload.Reset();
    m_heapManager = nullptr;
}

void VoxelWorld::SwapBuffers() {
    // Infinite streaming now keeps both dense buffers resident through the
    // toroidal copy path and brush writes. Local physics is in-place/budgeted,
    // not a complete READ->WRITE frame synthesis. Swapping here alternates
    // between two partially different worlds and makes painted geometry flicker.
    if (IsUsingInfiniteChunks()) {
        return;
    }

    // Do not block the CPU frame on chunk-copy completion. If copy work is still
    // in flight, keep the current read/write roles for one more frame and let
    // the renderer present the last stable buffer.
    if (m_chunkCopyFence && m_chunkCopyFenceValue > 0) {
        uint64_t completedValue = m_chunkCopyFence->GetCompletedValue();

        if (completedValue < m_chunkCopyFenceValue) {
            m_streamingStats.swapSkippedForCopyFence++;
            static uint32_t skippedSwapLogCounter = 0;
            if (++skippedSwapLogCounter % 120 == 1) {
                spdlog::debug("SwapBuffers: copy fence pending, keeping current read buffer (completed={}, expected={})",
                    completedValue, m_chunkCopyFenceValue);
            }
            return;
        }
    }

    if (!m_buffersStable) {
        static uint32_t skippedUnstableSwapLogCounter = 0;
        if (++skippedUnstableSwapLogCounter % 120 == 1) {
            spdlog::debug("SwapBuffers: streaming refill in progress, keeping current read buffer stable");
        }
        return;
    }

    int oldReadIndex = m_readBufferIndex;
    m_readBufferIndex = 1 - m_readBufferIndex;

    // DIAGNOSTIC: Log swap periodically
    static int swapCount = 0;
    if (++swapCount % 60 == 1) {
        spdlog::debug("SwapBuffers: {} -> {} (swap #{})", oldReadIndex, m_readBufferIndex, swapCount);
    }
}

void VoxelWorld::TransitionReadBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state) {
    m_voxelBuffers[m_readBufferIndex].TransitionTo(cmdList, state);
}

void VoxelWorld::TransitionWriteBufferTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state) {
    m_voxelBuffers[1 - m_readBufferIndex].TransitionTo(cmdList, state);
}

Result<void> VoxelWorld::CreateVoxelBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager) {
    uint64_t bufferSize = static_cast<uint64_t>(GetTotalVoxels()) * sizeof(uint32_t);

    // Create both ping-pong buffers with UAV support
    for (int i = 0; i < 2; i++) {
        auto result = m_voxelBuffers[i].Initialize(
            device,
            bufferSize,
            Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
            sizeof(uint32_t),  // stride
            i == 0 ? "VoxelBuffer_A" : "VoxelBuffer_B"
        );

        if (!result) {
            return Error("Failed to create voxel buffer {}: {}", i, result.error());
        }

        // Create SRV for reading (staging heap)
        result = m_voxelBuffers[i].CreateSRV(device, heapManager);
        if (!result) {
            return Error("Failed to create SRV for buffer {}: {}", i, result.error());
        }

        // Create UAV for writing (staging heap)
        result = m_voxelBuffers[i].CreateUAV(device, heapManager);
        if (!result) {
            return Error("Failed to create UAV for buffer {}: {}", i, result.error());
        }

        // Allocate persistent shader-visible SRV
        m_shaderVisibleSRVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_shaderVisibleSRVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible SRV for voxel buffer {}", i);
        }

        // Copy the SRV descriptor from staging to shader-visible heap
        device->CopyDescriptorsSimple(1,
            m_shaderVisibleSRVs[i].cpu,
            m_voxelBuffers[i].GetStagingSRV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Allocate persistent shader-visible UAV
        m_shaderVisibleUAVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_shaderVisibleUAVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible UAV for voxel buffer {}", i);
        }

        // Copy the UAV descriptor from staging to shader-visible heap
        device->CopyDescriptorsSimple(1,
            m_shaderVisibleUAVs[i].cpu,
            m_voxelBuffers[i].GetStagingUAV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        const uint64_t maskBytes =
            static_cast<uint64_t>(RENDER_BUFFER_CHUNKS_X) *
            static_cast<uint64_t>(RENDER_BUFFER_CHUNKS_Y) *
            static_cast<uint64_t>(RENDER_BUFFER_CHUNKS_Z) *
            sizeof(uint32_t) * 4u;
        result = m_chunkValidMasks[i].Initialize(
            device,
            maskBytes,
            Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
            sizeof(uint32_t) * 4u,
            i == 0 ? "ChunkSlotTags_A" : "ChunkSlotTags_B");
        if (!result) {
            return Error("Failed to create chunk valid mask {}: {}", i, result.error());
        }

        result = m_chunkValidMasks[i].CreateSRV(device, heapManager);
        if (!result) {
            return Error("Failed to create SRV for chunk valid mask {}: {}", i, result.error());
        }
        result = m_chunkValidMasks[i].CreateUAV(device, heapManager);
        if (!result) {
            return Error("Failed to create UAV for chunk valid mask {}: {}", i, result.error());
        }

        m_chunkValidMaskSRVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_chunkValidMaskSRVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible SRV for chunk valid mask {}", i);
        }
        device->CopyDescriptorsSimple(
            1,
            m_chunkValidMaskSRVs[i].cpu,
            m_chunkValidMasks[i].GetStagingSRV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        m_chunkValidMaskUAVs[i] = heapManager.AllocateShaderVisibleCbvSrvUav();
        if (!m_chunkValidMaskUAVs[i].IsValid()) {
            return Error("Failed to allocate shader-visible UAV for chunk valid mask {}", i);
        }
        device->CopyDescriptorsSimple(
            1,
            m_chunkValidMaskUAVs[i].cpu,
            m_chunkValidMasks[i].GetStagingUAV().cpu,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    const size_t slotCount =
        static_cast<size_t>(RENDER_BUFFER_CHUNKS_X) *
        static_cast<size_t>(RENDER_BUFFER_CHUNKS_Y) *
        static_cast<size_t>(RENDER_BUFFER_CHUNKS_Z);
    for (auto& slots : m_chunkSlotWorldCoords) {
        slots.assign(slotCount, ChunkCoord(INT32_MAX, INT32_MAX, INT32_MAX));
    }

    spdlog::debug("Created persistent shader-visible descriptors for voxel buffers");
    return {};
}

Result<void> VoxelWorld::CreateBrushEditFeedbackBuffers(ID3D12Device* device, Graphics::DescriptorHeapManager& heapManager) {
    const uint64_t eventBytes =
        static_cast<uint64_t>(MAX_BRUSH_EDIT_FEEDBACK_EVENTS) * sizeof(GpuBrushEditEvent);

    auto result = m_brushEditEventBuffer.Initialize(
        device,
        eventBytes,
        Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
        sizeof(GpuBrushEditEvent),
        "BrushEditFeedbackEvents"
    );
    if (!result) {
        return Error("Failed to create brush edit feedback event buffer: {}", result.error());
    }

    result = m_brushEditEventBuffer.CreateUAV(device, heapManager);
    if (!result) {
        return Error("Failed to create brush edit event UAV: {}", result.error());
    }

    result = m_brushEditCounterBuffer.Initialize(
        device,
        sizeof(uint32_t),
        Graphics::BufferUsage::StructuredBuffer | Graphics::BufferUsage::UnorderedAccess,
        sizeof(uint32_t),
        "BrushEditFeedbackCounter"
    );
    if (!result) {
        return Error("Failed to create brush edit counter buffer: {}", result.error());
    }

    result = m_brushEditCounterBuffer.CreateUAV(device, heapManager);
    if (!result) {
        return Error("Failed to create brush edit counter UAV: {}", result.error());
    }

    const uint32_t zero = 0;
    result = m_brushEditCounterResetUpload.Initialize(
        device,
        sizeof(uint32_t),
        Graphics::BufferUsage::Upload,
        sizeof(uint32_t),
        "BrushEditCounterResetUpload"
    );
    if (!result) {
        return Error("Failed to create brush edit counter reset upload: {}", result.error());
    }
    m_brushEditCounterResetUpload.Upload(&zero, sizeof(zero));

    for (uint32_t i = 0; i < BRUSH_EDIT_FEEDBACK_READBACK_SLOTS; ++i) {
        result = m_brushEditFeedbackSlots[i].eventReadback.Initialize(
            device,
            eventBytes,
            Graphics::BufferUsage::Readback,
            sizeof(GpuBrushEditEvent),
            "BrushEditFeedbackEventsReadback"
        );
        if (!result) {
            return Error("Failed to create brush edit event readback slot {}: {}", i, result.error());
        }

        result = m_brushEditFeedbackSlots[i].counterReadback.Initialize(
            device,
            sizeof(uint32_t),
            Graphics::BufferUsage::Readback,
            sizeof(uint32_t),
            "BrushEditFeedbackCounterReadback"
        );
        if (!result) {
            return Error("Failed to create brush edit counter readback slot {}: {}", i, result.error());
        }
    }

    m_brushEditFeedbackAvailable = true;
    spdlog::info("GPU brush edit feedback enabled: {} compact events, {} async readback slots",
        MAX_BRUSH_EDIT_FEEDBACK_EVENTS,
        BRUSH_EDIT_FEEDBACK_READBACK_SLOTS);
    return {};
}

Result<void> VoxelWorld::CreateMaterialPalette(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    Graphics::DescriptorHeapManager& heapManager)
{
    // Default material palette colors (256 entries)
    // Format: RGBA float4
    std::array<float, 256 * 4> paletteData = {};

    // Define some default materials
    auto setMaterial = [&](int id, float r, float g, float b, float a = 1.0f) {
        paletteData[id * 4 + 0] = r;
        paletteData[id * 4 + 1] = g;
        paletteData[id * 4 + 2] = b;
        paletteData[id * 4 + 3] = a;
    };

    // MAT_AIR (0) - Transparent
    setMaterial(0, 0.0f, 0.0f, 0.0f, 0.0f);

    // MAT_SAND (1) - Sandy beige
    setMaterial(1, 0.76f, 0.70f, 0.50f);

    // MAT_WATER (2) - deep teal transparent. Matches the ocean/near-sheet deep
    // family (ShadeWaterSurface deep ~(0.10,0.27,0.35)->(0.05,0.16,0.24)) so the
    // palette-sampled water paths (raw mid voxels, mid column/closure proxies,
    // far-height lakes) read as the same water as the analytic sheets instead of
    // bright cornflower blue.
    setMaterial(2, 0.07f, 0.20f, 0.28f, 0.7f);

    // MAT_STONE (3) - weathered warm rock
    setMaterial(3, 0.50f, 0.50f, 0.40f);

    // MAT_DIRT (4) - muted grass/soil
    setMaterial(4, 0.42f, 0.55f, 0.28f);

    // MAT_WOOD (5) - Wood brown
    setMaterial(5, 0.6f, 0.4f, 0.2f);

    // MAT_FIRE (6) - Orange/yellow
    setMaterial(6, 1.0f, 0.6f, 0.1f);

    // MAT_LAVA (7) - Red/orange
    setMaterial(7, 1.0f, 0.3f, 0.0f);

    // MAT_ICE (8) - Light blue
    setMaterial(8, 0.7f, 0.85f, 0.95f, 0.8f);

    // MAT_OIL (9) - Dark purple/black
    setMaterial(9, 0.15f, 0.1f, 0.2f, 0.9f);

    // MAT_GLASS (10) - Transparent white
    setMaterial(10, 0.9f, 0.95f, 1.0f, 0.3f);

    // MAT_SMOKE (11) - Gray semi-transparent
    setMaterial(11, 0.3f, 0.3f, 0.35f, 0.4f);

    // MAT_ACID (12) - Toxic green semi-transparent
    setMaterial(12, 0.2f, 0.9f, 0.2f, 0.6f);

    // MAT_HONEY (13) - Golden amber
    setMaterial(13, 0.95f, 0.75f, 0.2f, 0.8f);

    // MAT_CONCRETE (14) - Gray (starts flowing, hardens to stone-like)
    setMaterial(14, 0.6f, 0.6f, 0.65f, 1.0f);

    // MAT_GUNPOWDER (15) - Dark gray/black powder
    setMaterial(15, 0.2f, 0.2f, 0.25f, 1.0f);

    // MAT_CRYSTAL (16) - Purple crystalline
    setMaterial(16, 0.7f, 0.3f, 0.9f, 0.7f);

    // MAT_STEAM (17) - White/light gray transparent
    setMaterial(17, 0.9f, 0.95f, 1.0f, 0.3f);

    // MAT_BEDROCK (255) - Dark gray
    setMaterial(255, 0.2f, 0.2f, 0.2f);

    // Create 1D texture for palette
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    texDesc.Width = 256;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_materialPalette)
    );

    if (FAILED(hr)) {
        return Error("Failed to create material palette texture: 0x{:08X}", hr);
    }
    m_materialPalette->SetName(L"MaterialPalette");

    // Create upload buffer
    uint64_t uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_paletteUpload)
    );

    if (FAILED(hr)) {
        return Error("Failed to create palette upload buffer: 0x{:08X}", hr);
    }

    // Copy data to upload buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSize;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, nullptr);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 0};
    hr = m_paletteUpload->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return Error("Failed to map palette upload buffer: 0x{:08X}", hr);
    }

    memcpy(mappedData, paletteData.data(), 256 * 4 * sizeof(float));
    m_paletteUpload->Unmap(0, nullptr);

    // Copy from upload to texture
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_paletteUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_materialPalette.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition to shader resource state
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_materialPalette.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    cmdList->ResourceBarrier(1, &barrier);

    // Create SRV in staging heap
    m_paletteSRV = heapManager.AllocateStagingCbvSrvUav();
    if (!m_paletteSRV.IsValid()) {
        return Error("Failed to allocate palette SRV descriptor");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture1D.MostDetailedMip = 0;
    srvDesc.Texture1D.MipLevels = 1;

    device->CreateShaderResourceView(m_materialPalette.Get(), &srvDesc, m_paletteSRV.cpu);

    // Allocate shader-visible SRV for palette
    m_paletteShaderVisibleSRV = heapManager.AllocateShaderVisibleCbvSrvUav();
    if (!m_paletteShaderVisibleSRV.IsValid()) {
        return Error("Failed to allocate shader-visible SRV for material palette");
    }

    // Copy descriptor to shader-visible heap
    device->CopyDescriptorsSimple(1,
        m_paletteShaderVisibleSRV.cpu,
        m_paletteSRV.cpu,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    spdlog::debug("Material palette created with 256 colors (shader-visible descriptor allocated)");
    return {};
}

void VoxelWorld::RequestBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;
    if (!m_config.enableRaycastResultBuffers || !m_brushRaycastResult.GetResource()) return;

    ID3D12Device* device = nullptr;
    m_brushRaycastResult.GetResource()->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    // Create tiny 16-byte readback buffer if it doesn't exist
    if (!m_brushRaycastReadback) {
        constexpr uint64_t bufferSize = 16;  // 4 floats = 16 bytes (vs 32 MB!)

        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_brushRaycastReadback)
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create brush raycast readback buffer: 0x{:08X}", hr);
            device->Release();
            return;
        }

        m_brushRaycastReadback->SetName(L"BrushRaycastReadback");
        spdlog::debug("Created brush raycast readback buffer (16 bytes - 2,000,000x smaller!)");
    }

    device->Release();

    // Transition brush result buffer to copy source
    D3D12_RESOURCE_STATES currentState = m_brushRaycastResult.GetCurrentState();
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            currentState,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Copy tiny 16-byte GPU buffer to CPU readback buffer
    cmdList->CopyResource(m_brushRaycastReadback.Get(), m_brushRaycastResult.GetResource());

    // Transition back to UAV state for next raycast
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            currentState
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Map and read the result immediately (safe because readback is tiny)
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 16};
    HRESULT hr = m_brushRaycastReadback->Map(0, &readRange, &mappedData);
    if (SUCCEEDED(hr)) {
        float* data = static_cast<float*>(mappedData);
        m_brushRaycastCPU.posX = data[0];
        m_brushRaycastCPU.posY = data[1];
        m_brushRaycastCPU.posZ = data[2];

        // Unpack normal and validity flag
        uint32_t packed = DecodeVoxelRaycastPackedWord(data[3]);
        m_brushRaycastCPU.normalPacked = packed;
        m_brushRaycastCPU.hasValidPosition = (packed >> 6) & 1;

        D3D12_RANGE writeRange = {0, 0};
        m_brushRaycastReadback->Unmap(0, &writeRange);
    }
}

void VoxelWorld::RequestGroundRaycastReadback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;
    if (!m_config.enableRaycastResultBuffers || !m_groundRaycastResult.GetResource()) return;

    ID3D12Device* device = nullptr;
    m_groundRaycastResult.GetResource()->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    // Create tiny 16-byte readback buffer if it doesn't exist
    if (!m_groundRaycastReadback) {
        constexpr uint64_t bufferSize = 16;  // 4 floats = 16 bytes

        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_groundRaycastReadback)
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create ground raycast readback buffer: 0x{:08X}", hr);
            device->Release();
            return;
        }

        m_groundRaycastReadback->SetName(L"GroundRaycastReadback");
        spdlog::debug("Created ground raycast readback buffer (16 bytes)");
    }

    device->Release();

    // Transition ground result buffer to copy source
    D3D12_RESOURCE_STATES currentState = m_groundRaycastResult.GetCurrentState();
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_groundRaycastResult.GetResource(),
            currentState,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Copy tiny 16-byte GPU buffer to CPU readback buffer
    cmdList->CopyResource(m_groundRaycastReadback.Get(), m_groundRaycastResult.GetResource());

    // Transition back to UAV state for next raycast
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_groundRaycastResult.GetResource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            currentState
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Map and read the result immediately
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 16};
    HRESULT hr = m_groundRaycastReadback->Map(0, &readRange, &mappedData);
    if (SUCCEEDED(hr)) {
        float* data = static_cast<float*>(mappedData);
        m_groundRaycastCPU.posX = data[0];
        m_groundRaycastCPU.posY = data[1];
        m_groundRaycastCPU.posZ = data[2];

        // Unpack normal and validity flag
        uint32_t packed = DecodeVoxelRaycastPackedWord(data[3]);
        m_groundRaycastCPU.normalPacked = packed;
        m_groundRaycastCPU.hasValidPosition = (packed >> 6) & 1;

        D3D12_RANGE writeRange = {0, 0};
        m_groundRaycastReadback->Unmap(0, &writeRange);
    }
}

void VoxelWorld::QueueBrushRaycastReadback(ID3D12GraphicsCommandList* cmdList, uint32_t slotIndex) {
    if (!cmdList) return;
    if (!m_config.enableRaycastResultBuffers || !m_brushRaycastResult.GetResource()) return;
    const uint32_t queuedFrame = slotIndex;
    slotIndex %= RAYCAST_READBACK_SLOTS;

    ID3D12Device* device = nullptr;
    m_brushRaycastResult.GetResource()->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    if (!m_brushRaycastReadbackSlots[slotIndex]) {
        constexpr uint64_t bufferSize = 16;
        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_brushRaycastReadbackSlots[slotIndex])
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create brush raycast readback slot {}: 0x{:08X}", slotIndex, hr);
            device->Release();
            return;
        }

        m_brushRaycastReadbackSlots[slotIndex]->SetName(L"BrushRaycastReadbackSlot");
    }

    device->Release();

    D3D12_RESOURCE_STATES currentState = m_brushRaycastResult.GetCurrentState();
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            currentState,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    cmdList->CopyResource(m_brushRaycastReadbackSlots[slotIndex].Get(), m_brushRaycastResult.GetResource());

    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_brushRaycastResult.GetResource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            currentState
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    m_brushRaycastReadbackReady[slotIndex] = true;
    m_brushRaycastReadbackQueuedFrame[slotIndex] = queuedFrame;
}

bool VoxelWorld::RetireBrushRaycastReadback(uint32_t slotIndex) {
    const uint32_t retireFrame = slotIndex;
    slotIndex %= RAYCAST_READBACK_SLOTS;
    if (!m_config.enableRaycastResultBuffers) {
        return false;
    }
    if (!m_brushRaycastReadbackReady[slotIndex] || !m_brushRaycastReadbackSlots[slotIndex]) {
        return false;
    }
    if (!IsVoxelRaycastReadbackRetirable(m_brushRaycastReadbackQueuedFrame[slotIndex], retireFrame)) {
        return false;
    }

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 16};
    HRESULT hr = m_brushRaycastReadbackSlots[slotIndex]->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return false;
    }

    float* data = static_cast<float*>(mappedData);
    m_brushRaycastCPU.posX = data[0];
    m_brushRaycastCPU.posY = data[1];
    m_brushRaycastCPU.posZ = data[2];
    uint32_t packed = DecodeVoxelRaycastPackedWord(data[3]);
    m_brushRaycastCPU.normalPacked = packed;
    m_brushRaycastCPU.hasValidPosition = (packed >> 6) & 1;

    D3D12_RANGE writeRange = {0, 0};
    m_brushRaycastReadbackSlots[slotIndex]->Unmap(0, &writeRange);
    m_brushRaycastReadbackReady[slotIndex] = false;
    m_brushRaycastReadbackQueuedFrame[slotIndex] = InvalidVoxelRaycastReadbackFrame();
    return true;
}

void VoxelWorld::QueueGroundRaycastReadback(ID3D12GraphicsCommandList* cmdList, uint32_t slotIndex) {
    if (!cmdList) return;
    if (!m_config.enableRaycastResultBuffers || !m_groundRaycastResult.GetResource()) return;
    const uint32_t queuedFrame = slotIndex;
    slotIndex %= RAYCAST_READBACK_SLOTS;

    ID3D12Device* device = nullptr;
    m_groundRaycastResult.GetResource()->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return;

    if (!m_groundRaycastReadbackSlots[slotIndex]) {
        constexpr uint64_t bufferSize = 16;
        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_groundRaycastReadbackSlots[slotIndex])
        );

        if (FAILED(hr)) {
            spdlog::error("Failed to create ground raycast readback slot {}: 0x{:08X}", slotIndex, hr);
            device->Release();
            return;
        }

        m_groundRaycastReadbackSlots[slotIndex]->SetName(L"GroundRaycastReadbackSlot");
    }

    device->Release();

    D3D12_RESOURCE_STATES currentState = m_groundRaycastResult.GetCurrentState();
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_groundRaycastResult.GetResource(),
            currentState,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    cmdList->CopyResource(m_groundRaycastReadbackSlots[slotIndex].Get(), m_groundRaycastResult.GetResource());

    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_groundRaycastResult.GetResource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            currentState
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    m_groundRaycastReadbackReady[slotIndex] = true;
    m_groundRaycastReadbackQueuedFrame[slotIndex] = queuedFrame;
}

bool VoxelWorld::RetireGroundRaycastReadback(uint32_t slotIndex) {
    const uint32_t retireFrame = slotIndex;
    slotIndex %= RAYCAST_READBACK_SLOTS;
    if (!m_config.enableRaycastResultBuffers) {
        return false;
    }
    if (!m_groundRaycastReadbackReady[slotIndex] || !m_groundRaycastReadbackSlots[slotIndex]) {
        return false;
    }
    if (!IsVoxelRaycastReadbackRetirable(m_groundRaycastReadbackQueuedFrame[slotIndex], retireFrame)) {
        return false;
    }

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 16};
    HRESULT hr = m_groundRaycastReadbackSlots[slotIndex]->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        return false;
    }

    float* data = static_cast<float*>(mappedData);
    m_groundRaycastCPU.posX = data[0];
    m_groundRaycastCPU.posY = data[1];
    m_groundRaycastCPU.posZ = data[2];
    uint32_t packed = DecodeVoxelRaycastPackedWord(data[3]);
    m_groundRaycastCPU.normalPacked = packed;
    m_groundRaycastCPU.hasValidPosition = (packed >> 6) & 1;

    D3D12_RANGE writeRange = {0, 0};
    m_groundRaycastReadbackSlots[slotIndex]->Unmap(0, &writeRange);
    m_groundRaycastReadbackReady[slotIndex] = false;
    m_groundRaycastReadbackQueuedFrame[slotIndex] = InvalidVoxelRaycastReadbackFrame();
    return true;
}

glm::vec3 VoxelWorld::UpdateChunks(
    ID3D12Device* device,
    ID3D12CommandQueue* cmdQueue,  // CHANGED: Pass queue instead of cmdList
    const glm::vec3& cameraPos)
{
    if (!m_chunkManager || !m_useInfiniteChunks) {
        return glm::vec3(0.0f, 0.0f, 0.0f);  // Chunk system disabled, no origin shift
    }

    // Update chunk loading/unloading based on camera position
    m_chunkManager->Update(device, cmdQueue, cameraPos);
    // Keep generated-count decisions and copy decisions in the same fence
    // snapshot. Polling again later let chunks be copied before they were
    // included in lastVisibleGeneratedCount, which woke UpdateActiveRegion on
    // the next frame even though there was no new visible copy work left.
    m_chunkManager->PollCompletedChunks();

    // Calculate which chunk the camera is currently in. X/Z follow the stable
    // world camera chunk. Y is clamped to the generated terrain-backed range:
    // when the player flies far above a peak, the dense editable window should
    // keep the world below loaded instead of chasing the camera into empty
    // space and clearing/reloading every vertical chunk. The camera itself is
    // never clamped or mutated; it may raycast into the render AABB from above.
    ChunkCoord rawCameraChunk = ChunkCoord::FromWorldPosition(
        static_cast<int32_t>(std::floor(cameraPos.x)),
        static_cast<int32_t>(std::floor(cameraPos.y)),
        static_cast<int32_t>(std::floor(cameraPos.z)),
        INFINITE_CHUNK_SIZE
    );
    ChunkCoord cameraChunk = rawCameraChunk;
    cameraChunk.y = ClampVerticalChunkCenter(
        rawCameraChunk.y,
        RENDER_DISTANCE_VERTICAL_BELOW,
        RENDER_DISTANCE_VERTICAL_ABOVE);
    m_lastRenderCameraChunk = cameraChunk;

    // Update active region when the render-window policy requests a shift or
    // when new chunks finish generating.
    static size_t lastGeneratedCount = 0;
    static size_t lastVisibleGeneratedCount = 0;
    static uint32_t refillPulseCounter = 0;

    // FIX: Count GENERATED chunks only (not just queued/submitted chunks)
    // GetLoadedChunkCount() includes chunks that are queued but not generated yet,
    // so we need to manually count chunks in Generated or Dirty state
    size_t currentGeneratedCount = 0;
    for (const auto& entry : m_chunkManager->GetLoadedChunks()) {
        if (entry.second && entry.second->IsGenerated()) {
            currentGeneratedCount++;
        }
    }
    m_streamingStats.loadedChunkRecords = static_cast<uint32_t>(m_chunkManager->GetLoadedChunkCount());
    m_streamingStats.generatedChunks = static_cast<uint32_t>(currentGeneratedCount);
    m_streamingStats.queuedChunks = static_cast<uint32_t>(m_chunkManager->GetGenerationQueueSize());
    m_streamingStats.visibleVoxelCapacity = GetTotalVoxels64();
    m_streamingStats.loadedVoxelCapacity =
        static_cast<uint64_t>(currentGeneratedCount) *
        static_cast<uint64_t>(INFINITE_CHUNK_SIZE) *
        static_cast<uint64_t>(INFINITE_CHUNK_SIZE) *
        static_cast<uint64_t>(INFINITE_CHUNK_SIZE);
    const bool anyChunksGenerated = (currentGeneratedCount != lastGeneratedCount);

    // ============================================================================
    // CRITICAL FIX: Update activeRegionCenter with HYSTERESIS to prevent constant stuttering!
    //
    // Problem: Updating m_activeRegionCenter every chunk boundary (every 64 voxels) causes:
    // 1. RegionOrigin shifts by 64 voxels
    // 2. Cache invalidation forces re-copying ALL 1,250 chunks
    // 3. For 2-3 frames, buffer has mismatched data/origin -> flickering and visual jump
    // 4. This happens every 64 voxels -> constant stuttering
    //
    // Solution: keep a margin before X/Z recentring. Y recentres per chunk so
    // deep climbs and falls keep the player inside the vertical render window.
    // ============================================================================
    bool bufferNeedsShift = false;
    std::string recenterReason = "none";
    const glm::vec3 oldRegionOriginWorld = m_regionOriginWorld;

    // CRITICAL: First frame initialization - must set activeRegionCenter before anything else!
    // NOTE: On first frame, DON'T mark as buffer shift - just initialize center.
    // This allows normal swapping while chunks are generating, so we see terrain as soon
    // as any chunks are ready.
    if (m_activeRegionCenter.x == INT32_MAX) {
        m_activeRegionCenter = cameraChunk;
        // DON'T set bufferNeedsShift = true here - we want normal operation on first frame
        // The caches start empty, so all chunks will be copied naturally
        spdlog::debug("=== FIRST FRAME INITIALIZATION ===");
        spdlog::debug("Camera at world ({:.1f}, {:.1f}, {:.1f}) = raw chunk [{},{},{}], render chunk [{},{},{}]",
            cameraPos.x, cameraPos.y, cameraPos.z,
            rawCameraChunk.x, rawCameraChunk.y, rawCameraChunk.z,
            cameraChunk.x, cameraChunk.y, cameraChunk.z);
        spdlog::debug("Active region center set to chunk [{},{},{}]",
            m_activeRegionCenter.x, m_activeRegionCenter.y, m_activeRegionCenter.z);
    }
    else {
        // Calculate distance from camera to current buffer center
        int dx = cameraChunk.x - m_activeRegionCenter.x;
        int dz = cameraChunk.z - m_activeRegionCenter.z;
        int absDx = abs(dx);
        int absDz = abs(dz);

        // Keep horizontal hysteresis wide, but keep the vertical eye position
        // near the top quarter of the render window. The world spends more Y
        // chunks below the player than above them, so the useful view is mostly
        // below the camera. This also prevents high flight from looking at the
        // finite render volume from the outside.
        constexpr int SHIFT_THRESHOLD_XZ = 4;
        constexpr float TARGET_LOCAL_Y =
            static_cast<float>(RENDER_BUFFER_VOXELS_Y - CHUNK_SIZE_VOXELS);
        constexpr float SHIFT_LOCAL_Y_LOW =
            TARGET_LOCAL_Y - static_cast<float>(CHUNK_SIZE_VOXELS);
        constexpr float SHIFT_LOCAL_Y_HIGH =
            TARGET_LOCAL_Y + static_cast<float>(CHUNK_SIZE_VOXELS / 2);
        const glm::vec3 cameraLocalBeforeShift = cameraPos - m_regionOriginWorld;
        const float cameraLocalYBeforeShift = cameraLocalBeforeShift.y;
        const bool shiftYDown =
            cameraLocalYBeforeShift < SHIFT_LOCAL_Y_LOW &&
            m_activeRegionCenter.y > cameraChunk.y;
        const bool shiftYUp =
            cameraLocalYBeforeShift > SHIFT_LOCAL_Y_HIGH &&
            m_activeRegionCenter.y < cameraChunk.y;

        const bool wantsRecenter = absDx > SHIFT_THRESHOLD_XZ || absDz > SHIFT_THRESHOLD_XZ || shiftYDown || shiftYUp;
        constexpr float HARD_LOCAL_MARGIN_XZ = static_cast<float>(CHUNK_SIZE_VOXELS * 2);
        constexpr float HARD_LOCAL_MARGIN_Y = static_cast<float>(CHUNK_SIZE_VOXELS);
        const bool cameraNearRenderEdgeXZ =
            cameraLocalBeforeShift.x < HARD_LOCAL_MARGIN_XZ ||
            cameraLocalBeforeShift.x > static_cast<float>(m_config.gridSizeX) - HARD_LOCAL_MARGIN_XZ ||
            cameraLocalBeforeShift.z < HARD_LOCAL_MARGIN_XZ ||
            cameraLocalBeforeShift.z > static_cast<float>(m_config.gridSizeZ) - HARD_LOCAL_MARGIN_XZ;
        const bool cameraNearRenderEdgeY =
            cameraLocalBeforeShift.y < HARD_LOCAL_MARGIN_Y ||
            cameraLocalBeforeShift.y > static_cast<float>(m_config.gridSizeY) - HARD_LOCAL_MARGIN_Y;
        const bool cameraNearRenderEdge = cameraNearRenderEdgeXZ || cameraNearRenderEdgeY;
        if (wantsRecenter && !m_buffersStable && !cameraNearRenderEdge) {
            static uint32_t deferredRecenterLogCounter = 0;
            if (++deferredRecenterLogCounter % 120 == 1) {
                spdlog::debug("Render-window recenter deferred while refill is incomplete (camera=[{},{},{}], active=[{},{},{}], dx={}, dz={}, localY={:.1f})",
                    cameraChunk.x, cameraChunk.y, cameraChunk.z,
                    m_activeRegionCenter.x, m_activeRegionCenter.y, m_activeRegionCenter.z,
                    dx, dz, cameraLocalYBeforeShift);
            }
        }
        else if (wantsRecenter) {
            ChunkCoord oldCenter = m_activeRegionCenter;
            recenterReason.clear();

            // Move gradually during normal walking, but catch up faster when
            // the camera is at/over a dense-buffer edge. A strict one-chunk
            // step lets fast flight outrun the render window for multiple
            // frames, which is the visible "floating island" failure.
            const int32_t maxShiftChunksXZ = cameraNearRenderEdgeXZ ? 4 : 1;
            const int32_t maxShiftChunksY = cameraNearRenderEdgeY ? 4 : 1;
            if (dx > SHIFT_THRESHOLD_XZ) {
                const int32_t shift = std::min<int32_t>(dx - SHIFT_THRESHOLD_XZ, maxShiftChunksXZ);
                m_activeRegionCenter.x += std::max<int32_t>(1, shift);
                recenterReason += "x+";
            } else if (dx < -SHIFT_THRESHOLD_XZ) {
                const int32_t shift = std::min<int32_t>((-dx) - SHIFT_THRESHOLD_XZ, maxShiftChunksXZ);
                m_activeRegionCenter.x -= std::max<int32_t>(1, shift);
                recenterReason += "x-";
            }

            if (shiftYUp) {
                const float excess = cameraLocalYBeforeShift - TARGET_LOCAL_Y;
                const int32_t shift = std::min<int32_t>(
                    static_cast<int32_t>(std::ceil(std::max(0.0f, excess) / static_cast<float>(CHUNK_SIZE_VOXELS))),
                    maxShiftChunksY);
                m_activeRegionCenter.y += std::max<int32_t>(1, shift);
                if (!recenterReason.empty()) recenterReason += ",";
                recenterReason += "y+";
            } else if (shiftYDown) {
                const float excess = TARGET_LOCAL_Y - cameraLocalYBeforeShift;
                const int32_t shift = std::min<int32_t>(
                    static_cast<int32_t>(std::ceil(std::max(0.0f, excess) / static_cast<float>(CHUNK_SIZE_VOXELS))),
                    maxShiftChunksY);
                m_activeRegionCenter.y -= std::max<int32_t>(1, shift);
                if (!recenterReason.empty()) recenterReason += ",";
                recenterReason += "y-";
            }

            if (dz > SHIFT_THRESHOLD_XZ) {
                const int32_t shift = std::min<int32_t>(dz - SHIFT_THRESHOLD_XZ, maxShiftChunksXZ);
                m_activeRegionCenter.z += std::max<int32_t>(1, shift);
                if (!recenterReason.empty()) recenterReason += ",";
                recenterReason += "z+";
            } else if (dz < -SHIFT_THRESHOLD_XZ) {
                const int32_t shift = std::min<int32_t>((-dz) - SHIFT_THRESHOLD_XZ, maxShiftChunksXZ);
                m_activeRegionCenter.z -= std::max<int32_t>(1, shift);
                if (!recenterReason.empty()) recenterReason += ",";
                recenterReason += "z-";
            }
            if (recenterReason.empty()) {
                recenterReason = "threshold";
            }
            if (!m_buffersStable && cameraNearRenderEdge) {
                recenterReason += "+edge";
            }

            bufferNeedsShift = true;
            spdlog::debug("Buffer center shifted from chunk [{},{},{}] to [{},{},{}] (raw camera at [{},{},{}], render camera at [{},{},{}], localYBefore={:.1f}, reason={})",
                oldCenter.x, oldCenter.y, oldCenter.z,
                m_activeRegionCenter.x, m_activeRegionCenter.y, m_activeRegionCenter.z,
                rawCameraChunk.x, rawCameraChunk.y, rawCameraChunk.z,
                cameraChunk.x, cameraChunk.y, cameraChunk.z,
                cameraLocalYBeforeShift,
                recenterReason);
        }
    }

    // ============================================================================
    // regionOrigin tells shader where buffer position (0,0,0) is in world coords
    //
    // Chunks are copied to buffer with offset:
    //   destX = (chunkCoord.x - activeRegionCenter.x + RENDER_DISTANCE) * 64
    //
    // So buffer (0,0,0) = world chunk (activeRegionCenter - RENDER_DISTANCE)
    //                   = world voxel (activeRegionCenter - RENDER_DISTANCE) * 64
    // ============================================================================
    m_regionOriginWorld = glm::vec3(
        static_cast<float>((m_activeRegionCenter.x - RENDER_DISTANCE_HORIZONTAL) * CHUNK_SIZE_VOXELS),
        static_cast<float>((m_activeRegionCenter.y - RENDER_DISTANCE_VERTICAL_BELOW) * CHUNK_SIZE_VOXELS),
        static_cast<float>((m_activeRegionCenter.z - RENDER_DISTANCE_HORIZONTAL) * CHUNK_SIZE_VOXELS)
    );
    m_streamingStats.activeChunkX = m_activeRegionCenter.x;
    m_streamingStats.activeChunkY = m_activeRegionCenter.y;
    m_streamingStats.activeChunkZ = m_activeRegionCenter.z;
    m_streamingStats.renderMinY = static_cast<int32_t>(m_regionOriginWorld.y);
    m_streamingStats.renderMaxY = m_streamingStats.renderMinY + static_cast<int32_t>(m_config.gridSizeY) - 1;
    m_chunkManager->EnsureQueuedAroundChunk(m_activeRegionCenter);
    glm::vec3 originShiftDelta = bufferNeedsShift
        ? (m_regionOriginWorld - oldRegionOriginWorld)
        : glm::vec3(0.0f);

    // Log first frame region origin (critical for debugging camera position issues)
    if (!m_firstUpdateDone) {
        spdlog::debug("Region origin set to world voxel ({},{},{})",
            static_cast<int>(m_regionOriginWorld.x),
            static_cast<int>(m_regionOriginWorld.y),
            static_cast<int>(m_regionOriginWorld.z));
        glm::vec3 expectedBufferPos = cameraPos - m_regionOriginWorld;
        spdlog::debug("Camera buffer position should be ({:.1f},{:.1f},{:.1f}) inside the moving render buffer",
            expectedBufferPos.x, expectedBufferPos.y, expectedBufferPos.z);
    }

    // Log when region origin shifts (only when buffer actually shifts, not every chunk change)
    if (bufferNeedsShift) {
        m_streamingStats.lastRecenterDeltaX = static_cast<int32_t>(originShiftDelta.x);
        m_streamingStats.lastRecenterDeltaY = static_cast<int32_t>(originShiftDelta.y);
        m_streamingStats.lastRecenterDeltaZ = static_cast<int32_t>(originShiftDelta.z);
        m_streamingStats.lastRecenterOldOriginX = static_cast<int32_t>(oldRegionOriginWorld.x);
        m_streamingStats.lastRecenterOldOriginY = static_cast<int32_t>(oldRegionOriginWorld.y);
        m_streamingStats.lastRecenterOldOriginZ = static_cast<int32_t>(oldRegionOriginWorld.z);
        m_streamingStats.lastRecenterNewOriginX = static_cast<int32_t>(m_regionOriginWorld.x);
        m_streamingStats.lastRecenterNewOriginY = static_cast<int32_t>(m_regionOriginWorld.y);
        m_streamingStats.lastRecenterNewOriginZ = static_cast<int32_t>(m_regionOriginWorld.z);
        m_streamingStats.lastRecenterFrame++;
        m_streamingStats.lastRecenterPlayerChanged = 0;
        std::snprintf(m_streamingStats.lastRecenterReason,
            sizeof(m_streamingStats.lastRecenterReason),
            "%s",
            recenterReason.c_str());

        spdlog::debug("Region recenter reason={} oldOrigin=({:.0f},{:.0f},{:.0f}) newOrigin=({:.0f},{:.0f},{:.0f}) delta=({:.0f},{:.0f},{:.0f}) playerWorld=({:.2f},{:.2f},{:.2f}) playerLocalBefore=({:.2f},{:.2f},{:.2f}) playerLocalAfter=({:.2f},{:.2f},{:.2f})",
            recenterReason,
            oldRegionOriginWorld.x, oldRegionOriginWorld.y, oldRegionOriginWorld.z,
            m_regionOriginWorld.x, m_regionOriginWorld.y, m_regionOriginWorld.z,
            originShiftDelta.x, originShiftDelta.y, originShiftDelta.z,
            cameraPos.x, cameraPos.y, cameraPos.z,
            cameraPos.x - oldRegionOriginWorld.x,
            cameraPos.y - oldRegionOriginWorld.y,
            cameraPos.z - oldRegionOriginWorld.z,
            cameraPos.x - m_regionOriginWorld.x,
            cameraPos.y - m_regionOriginWorld.y,
            cameraPos.z - m_regionOriginWorld.z);
        spdlog::debug("Region origin shifted to world voxel ({},{},{}) - Cache invalidated",
            static_cast<int>(m_regionOriginWorld.x),
            static_cast<int>(m_regionOriginWorld.y),
            static_cast<int>(m_regionOriginWorld.z));
    }

    // DIAGNOSTIC: Log camera position and chunk periodically
    static int diagFrameCount = 0;
    static bool firstFrameLogged = false;
    if (++diagFrameCount >= 60 || !firstFrameLogged) {
        // Calculate what the shader will compute
        glm::vec3 bufferPos = cameraPos - m_regionOriginWorld;
        spdlog::debug("[CAMERA DIAG] Pos=({:.1f},{:.1f},{:.1f}) RawChunk=[{},{},{}] RenderChunk=[{},{},{}] RegionOrigin=({:.0f},{:.0f},{:.0f}) BufferPos=({:.1f},{:.1f},{:.1f})",
            cameraPos.x, cameraPos.y, cameraPos.z,
            rawCameraChunk.x, rawCameraChunk.y, rawCameraChunk.z,
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            m_regionOriginWorld.x, m_regionOriginWorld.y, m_regionOriginWorld.z,
            bufferPos.x, bufferPos.y, bufferPos.z);
        diagFrameCount = 0;
        firstFrameLogged = true;
    }

    // Track if this is the first update (need to force UpdateActiveRegion on startup)
    const bool initialRenderBufferFill = !m_firstUpdateDone;
    const size_t expectedVisibleChunks =
        static_cast<size_t>((2 * RENDER_DISTANCE_HORIZONTAL + 1) *
                            RENDER_BUFFER_CHUNKS_Y *
                            (2 * RENDER_DISTANCE_HORIZONTAL + 1));
    size_t currentVisibleGeneratedCount = 0;
    if (m_activeRegionCenter.x != INT32_MAX) {
        const auto& loadedChunks = m_chunkManager->GetLoadedChunks();
        const int32_t renderChunkMinY = m_activeRegionCenter.y - RENDER_DISTANCE_VERTICAL_BELOW;
        const int32_t renderChunkMaxY = m_activeRegionCenter.y + RENDER_DISTANCE_VERTICAL_ABOVE;
        for (int32_t dz = -RENDER_DISTANCE_HORIZONTAL; dz <= RENDER_DISTANCE_HORIZONTAL; ++dz) {
            for (int32_t y = renderChunkMinY; y <= renderChunkMaxY; ++y) {
                for (int32_t dx = -RENDER_DISTANCE_HORIZONTAL; dx <= RENDER_DISTANCE_HORIZONTAL; ++dx) {
                    const ChunkCoord visibleCoord{
                        m_activeRegionCenter.x + dx,
                        y,
                        m_activeRegionCenter.z + dz
                    };
                    auto it = loadedChunks.find(visibleCoord);
                    if (it != loadedChunks.end() && it->second && it->second->IsGenerated()) {
                        ++currentVisibleGeneratedCount;
                    }
                }
            }
        }
    }
    const size_t visibleGeneratedDelta =
        currentVisibleGeneratedCount > lastVisibleGeneratedCount
            ? currentVisibleGeneratedCount - lastVisibleGeneratedCount
            : lastVisibleGeneratedCount - currentVisibleGeneratedCount;
    const bool visibleGeneratedChanged = currentVisibleGeneratedCount != lastVisibleGeneratedCount;
    const bool refillPulseDue = !m_buffersStable && ((++refillPulseCounter % 4u) == 0u);
    const bool visibleChunksGenerated =
        initialRenderBufferFill ||
        bufferNeedsShift ||
        visibleGeneratedChanged ||
        visibleGeneratedDelta >= 24 ||
        (refillPulseDue && currentVisibleGeneratedCount != lastVisibleGeneratedCount);
    const bool renderCachesIncomplete =
        m_firstUpdateDone &&
        (m_streamingStats.expectedVisibleChunks == 0 ||
         m_streamingStats.cachedReadChunks < expectedVisibleChunks ||
         m_streamingStats.cachedWriteChunks < expectedVisibleChunks);
    const bool visibleDemandPending =
        m_firstUpdateDone &&
        (m_streamingStats.readSlotMismatches > 0 ||
         m_streamingStats.writeSlotMismatches > 0 ||
         m_streamingStats.chunksNotLoadedLastFrame > 0 ||
         m_streamingStats.chunksNotGeneratedLastFrame > 0);
    const bool forcedRefresh = m_forcedActiveRegionUpdateFrames > 0;
    bool needsUpdate =
        bufferNeedsShift ||
        visibleChunksGenerated ||
        initialRenderBufferFill ||
        renderCachesIncomplete ||
        visibleDemandPending ||
        forcedRefresh;

    // Update if buffer needs shift, new chunks were generated, first frame, or
    // the ping-pong buffers are still converging. Without this path, READ could
    // partially fill, WRITE could stay empty, or the final visible slots could
    // remain permanent air holes after the critical-coverage gate opened.
    if (needsUpdate) {
        m_firstUpdateDone = true;
        // CRITICAL FIX: When buffer center shifts, buffer layout changes!
        // All cached chunk positions become INVALID because destX/Y/Z calculations
        // depend on m_activeRegionCenter.
        // With gradual 1-chunk shifts, the visual impact is minimal.
        if (bufferNeedsShift) {
            // Toroidal chunk slots preserve overlapping world chunks across
            // recentering. Do not clear the caches or valid tags here; newly
            // exposed edge chunks overwrite only their modulo slots.
            m_buffersStable = false;
            m_framesAfterCacheInvalidation = 24;

            // Persistent edit overlays own user changes; local modified flags
            // are only transient dense-buffer dirtiness markers.
            if (!m_modifiedChunks.empty()) {
                spdlog::debug("Buffer shift: clearing {} modified chunks", m_modifiedChunks.size());
                m_modifiedChunks.clear();
            }

            spdlog::debug("Buffer shift: toroidal slots preserved, edge chunks will be copied");
        }

        lastGeneratedCount = currentGeneratedCount;
        lastVisibleGeneratedCount = currentVisibleGeneratedCount;

        if (initialRenderBufferFill) {
            m_buffersStable = false;
            m_framesAfterCacheInvalidation = 24;
        }

        // Clear only on the first fill. Recenter is handled by toroidal
        // per-slot world tags, so clearing on movement would reintroduce the
        // full-window flash this cache is designed to remove.
        UpdateActiveRegion(device, cmdQueue, initialRenderBufferFill);
        if (m_forcedActiveRegionUpdateFrames > 0) {
            --m_forcedActiveRegionUpdateFrames;
        }

        if (bufferNeedsShift) {
            spdlog::debug("Camera at chunk [{},{},{}] - updating active region (buffer shifted)",
                cameraChunk.x, cameraChunk.y, cameraChunk.z);
        }
        // if (visibleChunksGenerated) {
        //     spdlog::debug("New chunks generated ({} total GENERATED, {} total loaded) - updating active region",
        //         currentGeneratedCount, m_chunkManager->GetLoadedChunkCount());
        // }
    }
    else if (anyChunksGenerated) {
        lastGeneratedCount = currentGeneratedCount;
        lastVisibleGeneratedCount = currentVisibleGeneratedCount;
        m_streamingStats.copyBudget = m_maxChunkCopiesPerFrame;
        m_streamingStats.chunksCopiedLastFrame = 0;
        m_streamingStats.chunksSkippedLastFrame = 0;
        m_streamingStats.chunksNotGeneratedLastFrame = 0;
        m_streamingStats.chunksNotLoadedLastFrame = 0;
        m_streamingStats.urgentVisibleChunksQueuedLastFrame = 0;
        m_streamingStats.chunksCheckedLastFrame = 0;
    }
    else {
        m_streamingStats.copyBudget = m_maxChunkCopiesPerFrame;
        m_streamingStats.chunksCopiedLastFrame = 0;
        m_streamingStats.chunksSkippedLastFrame = 0;
        m_streamingStats.chunksNotGeneratedLastFrame = 0;
        m_streamingStats.chunksNotLoadedLastFrame = 0;
        m_streamingStats.urgentVisibleChunksQueuedLastFrame = 0;
        m_streamingStats.chunksCheckedLastFrame = 0;
    }

    // Log chunk changes for debugging
    static ChunkCoord lastLoggedChunk = {INT32_MAX, INT32_MAX, INT32_MAX};
    if (rawCameraChunk != lastLoggedChunk) {
        const auto& loadedChunks = m_chunkManager->GetLoadedChunks();
        spdlog::debug("Camera entered raw chunk [{},{},{}] render chunk [{},{},{}] - {} chunks loaded",
            rawCameraChunk.x, rawCameraChunk.y, rawCameraChunk.z,
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            loadedChunks.size());
        lastLoggedChunk = rawCameraChunk;
    }

    if (glm::length(originShiftDelta) > 0.01f) {
        spdlog::debug("Origin shifted by ({:.1f}, {:.1f}, {:.1f}); player/camera world position remains unchanged",
            originShiftDelta.x, originShiftDelta.y, originShiftDelta.z);
    }
    return originShiftDelta;
}

void VoxelWorld::OnChunkUnloaded(const ChunkCoord& coord) {
    // CACHE FIX: Clear this chunk from BOTH buffer caches when it's unloaded
    // Without this, if the chunk gets reloaded later, UpdateActiveRegion thinks
    // it's already in the render buffer and skips copying it -> invisible chunk!
    for (int i = 0; i < 2; ++i) {
        auto it = m_copiedChunksPerBuffer[i].find(coord);
        if (it != m_copiedChunksPerBuffer[i].end()) {
            m_copiedChunksPerBuffer[i].erase(it);
        }
    }

    // Clear only the legacy render-buffer marker. Sparse persistent edit overlays
    // remain in m_editOverlays so the chunk can be regenerated and replayed later.
    m_modifiedChunks.erase(coord);

    spdlog::trace("Cleared chunk [{},{},{}] from copy caches on unload",
        coord.x, coord.y, coord.z);
}

void VoxelWorld::InvalidateCopiedChunk(const ChunkCoord& coord) {
    // Legacy marker retained for diagnostics and compatibility with older call
    // sites. Persistent edits are now stored in m_editOverlays and re-applied
    // after chunk copy, so UpdateActiveRegion no longer uses this set to block
    // copies from generated chunks.
    m_modifiedChunks.insert(coord);

    spdlog::debug("Marked chunk [{},{},{}] as modified by brush",
        coord.x, coord.y, coord.z);
}

void VoxelWorld::StorePersistentVoxelEdit(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
    const ChunkCoord coord = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    const uint32_t localX = static_cast<uint32_t>(FloorMod(worldX, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localY = static_cast<uint32_t>(FloorMod(worldY, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localZ = static_cast<uint32_t>(FloorMod(worldZ, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localIndex = LocalVoxelIndex(localX, localY, localZ);

    auto& overlay = m_editOverlays[coord];
    const bool inserted = overlay.voxels.find(localIndex) == overlay.voxels.end();
    overlay.voxels[localIndex] = packedVoxel;
    if (inserted) {
        ++m_totalEditedVoxels;
    }

    m_modifiedChunks.insert(coord);
    RequestActiveRegionRefresh(12);
}

bool VoxelWorld::TryGetPersistentVoxelEdit(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t* outVoxel) const {
    const ChunkCoord coord = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    const uint32_t localX = static_cast<uint32_t>(FloorMod(worldX, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localY = static_cast<uint32_t>(FloorMod(worldY, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localZ = static_cast<uint32_t>(FloorMod(worldZ, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    const uint32_t localIndex = LocalVoxelIndex(localX, localY, localZ);

    auto overlayIt = m_editOverlays.find(coord);
    if (overlayIt == m_editOverlays.end()) {
        return false;
    }

    auto voxelIt = overlayIt->second.voxels.find(localIndex);
    if (voxelIt == overlayIt->second.voxels.end()) {
        return false;
    }

    if (outVoxel) {
        *outVoxel = voxelIt->second;
    }
    return true;
}

bool VoxelWorld::HasPersistentEditAtWorldVoxel(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    return TryGetPersistentVoxelEdit(worldX, worldY, worldZ, nullptr);
}

void VoxelWorld::UpdateTargetVoxelDebug(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    int32_t normalX,
    int32_t normalY,
    int32_t normalZ)
{
    const ChunkCoord coord = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    m_streamingStats.targetWorldX = worldX;
    m_streamingStats.targetWorldY = worldY;
    m_streamingStats.targetWorldZ = worldZ;
    m_streamingStats.targetChunkX = coord.x;
    m_streamingStats.targetChunkY = coord.y;
    m_streamingStats.targetChunkZ = coord.z;
    m_streamingStats.targetLocalX = static_cast<uint32_t>(FloorMod(worldX, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    m_streamingStats.targetLocalY = static_cast<uint32_t>(FloorMod(worldY, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    m_streamingStats.targetLocalZ = static_cast<uint32_t>(FloorMod(worldZ, static_cast<int32_t>(INFINITE_CHUNK_SIZE)));
    m_streamingStats.targetNormalX = normalX;
    m_streamingStats.targetNormalY = normalY;
    m_streamingStats.targetNormalZ = normalZ;
    m_streamingStats.targetHasPersistentEdit = HasPersistentEditAtWorldVoxel(worldX, worldY, worldZ) ? 1u : 0u;
}

void VoxelWorld::RefreshPersistentEditStats() {
    m_streamingStats.editedChunks = static_cast<uint32_t>(m_editOverlays.size());
    m_streamingStats.editedVoxels = m_totalEditedVoxels;
}

void VoxelWorld::ResetBrushFeedbackStats() {
    m_streamingStats.gpuBrushEventsAppliedLastFrame = 0;
    m_streamingStats.gpuBrushEventsDroppedLastFrame = 0;
    m_streamingStats.gpuBrushEventsOverflowLastFrame = 0;

    uint32_t pendingSlots = 0;
    for (const auto& slot : m_brushEditFeedbackSlots) {
        if (slot.pending) {
            ++pendingSlots;
        }
    }
    m_streamingStats.gpuBrushFeedbackPending = pendingSlots;
}

bool VoxelWorld::BeginBrushEditFeedback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList || !m_brushEditFeedbackAvailable || !IsUsingInfiniteChunks()) {
        return false;
    }

    if (m_activeBrushEditFeedbackSlot >= 0) {
        return false;
    }

    int32_t selectedSlot = -1;
    for (uint32_t attempt = 0; attempt < BRUSH_EDIT_FEEDBACK_READBACK_SLOTS; ++attempt) {
        const uint32_t slotIndex = (m_nextBrushEditFeedbackSlot + attempt) % BRUSH_EDIT_FEEDBACK_READBACK_SLOTS;
        if (!m_brushEditFeedbackSlots[slotIndex].pending) {
            selectedSlot = static_cast<int32_t>(slotIndex);
            m_nextBrushEditFeedbackSlot = (slotIndex + 1) % BRUSH_EDIT_FEEDBACK_READBACK_SLOTS;
            break;
        }
    }

    if (selectedSlot < 0) {
        ++m_streamingStats.gpuBrushEventsDroppedLastFrame;
        spdlog::warn("Brush edit feedback skipped: all readback slots are pending");
        return false;
    }

    m_brushEditCounterBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(
        m_brushEditCounterBuffer.GetResource(),
        0,
        m_brushEditCounterResetUpload.GetResource(),
        0,
        sizeof(uint32_t));
    m_brushEditCounterBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_brushEditEventBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto& slot = m_brushEditFeedbackSlots[static_cast<uint32_t>(selectedSlot)];
    slot.pending = true;
    slot.fenceValue = 0;
    // Brush feedback is recorded from the READ pass because READ is the
    // authoritative visible cache in infinite streaming mode. Persisting this
    // buffer's exact shader writes prevents feedback from following a stale or
    // divergent WRITE slot.
    slot.bufferIndex = m_readBufferIndex;
    slot.regionOriginWorld = m_regionOriginWorld;
    m_activeBrushEditFeedbackSlot = selectedSlot;
    m_streamingStats.gpuBrushFeedbackQueued++;
    uint32_t pendingSlots = 0;
    for (const auto& pendingSlot : m_brushEditFeedbackSlots) {
        if (pendingSlot.pending) {
            ++pendingSlots;
        }
    }
    m_streamingStats.gpuBrushFeedbackPending = pendingSlots;
    return true;
}

void VoxelWorld::QueueBrushEditFeedbackReadback(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList || m_activeBrushEditFeedbackSlot < 0 || !m_brushEditFeedbackAvailable) {
        return;
    }

    auto& slot = m_brushEditFeedbackSlots[static_cast<uint32_t>(m_activeBrushEditFeedbackSlot)];

    m_brushEditEventBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_brushEditCounterBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    cmdList->CopyBufferRegion(
        slot.eventReadback.GetResource(),
        0,
        m_brushEditEventBuffer.GetResource(),
        0,
        m_brushEditEventBuffer.GetSize());
    cmdList->CopyBufferRegion(
        slot.counterReadback.GetResource(),
        0,
        m_brushEditCounterBuffer.GetResource(),
        0,
        sizeof(uint32_t));

    m_brushEditEventBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_brushEditCounterBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void VoxelWorld::NotifyBrushEditFeedbackFence(uint64_t fenceValue) {
    if (m_activeBrushEditFeedbackSlot < 0) {
        return;
    }

    auto& slot = m_brushEditFeedbackSlots[static_cast<uint32_t>(m_activeBrushEditFeedbackSlot)];
    slot.fenceValue = fenceValue;
    m_activeBrushEditFeedbackSlot = -1;
}

void VoxelWorld::RetireBrushEditFeedback(uint64_t completedFenceValue) {
    ResetBrushFeedbackStats();
    if (!m_brushEditFeedbackAvailable) {
        return;
    }

    for (auto& slot : m_brushEditFeedbackSlots) {
        if (!slot.pending || slot.fenceValue == 0 || slot.fenceValue > completedFenceValue) {
            continue;
        }

        uint32_t* counter = static_cast<uint32_t*>(slot.counterReadback.Map());
        const GpuBrushEditEvent* events = static_cast<const GpuBrushEditEvent*>(slot.eventReadback.Map());
        if (!counter || !events) {
            slot.pending = false;
            continue;
        }

        const uint32_t eventCount = *counter;
        const uint32_t clampedEventCount = std::min(eventCount, MAX_BRUSH_EDIT_FEEDBACK_EVENTS);
        const uint32_t overflowCount = eventCount - clampedEventCount;
        uint32_t appliedCount = 0;

        for (uint32_t i = 0; i < clampedEventCount; ++i) {
            const auto& event = events[i];
            if (event.localRenderIndex >= GetTotalVoxels()) {
                ++m_streamingStats.gpuBrushEventsDroppedLastFrame;
                continue;
            }

            const uint32_t localX = event.localRenderIndex % m_config.gridSizeX;
            const uint32_t localY = (event.localRenderIndex / m_config.gridSizeX) % m_config.gridSizeY;
            const uint32_t localZ = event.localRenderIndex / (m_config.gridSizeX * m_config.gridSizeY);
            const uint32_t slotX = localX / CHUNK_SIZE_VOXELS;
            const uint32_t slotY = localY / CHUNK_SIZE_VOXELS;
            const uint32_t slotZ = localZ / CHUNK_SIZE_VOXELS;
            const uint32_t slotIndex =
                slotX +
                slotY * RENDER_BUFFER_CHUNKS_X +
                slotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
            if (slot.bufferIndex >= m_chunkSlotWorldCoords.size() ||
                slotIndex >= m_chunkSlotWorldCoords[slot.bufferIndex].size()) {
                ++m_streamingStats.gpuBrushEventsDroppedLastFrame;
                continue;
            }
            const ChunkCoord worldChunk = m_chunkSlotWorldCoords[slot.bufferIndex][slotIndex];
            if (worldChunk.x == INT32_MAX) {
                ++m_streamingStats.gpuBrushEventsDroppedLastFrame;
                continue;
            }
            const int32_t worldX = worldChunk.x * CHUNK_SIZE_VOXELS + static_cast<int32_t>(localX % CHUNK_SIZE_VOXELS);
            const int32_t worldY = worldChunk.y * CHUNK_SIZE_VOXELS + static_cast<int32_t>(localY % CHUNK_SIZE_VOXELS);
            const int32_t worldZ = worldChunk.z * CHUNK_SIZE_VOXELS + static_cast<int32_t>(localZ % CHUNK_SIZE_VOXELS);

            StorePersistentVoxelEdit(worldX, worldY, worldZ, event.packedVoxel);
            ++appliedCount;
        }

        m_streamingStats.gpuBrushEventsAppliedLastFrame += appliedCount;
        m_streamingStats.persistentEditsRecordedLastStroke = appliedCount;
        m_streamingStats.brushVoxelsEvaluatedLastStroke = clampedEventCount;
        m_streamingStats.brushVoxelsRejectedLastStroke = 0;
        m_streamingStats.gpuBrushEventsOverflowLastFrame += overflowCount;
        if (overflowCount > 0) {
            spdlog::warn("Brush edit feedback overflow: {} events dropped after {} recorded",
                overflowCount,
                MAX_BRUSH_EDIT_FEEDBACK_EVENTS);
        }
        if (appliedCount > 0) {
            static uint32_t feedbackLogThrottle = 0;
            if ((++feedbackLogThrottle % 60) == 1) {
                spdlog::debug("GPU brush edit feedback applied {} events at region origin ({:.0f}, {:.0f}, {:.0f})",
                    appliedCount,
                    slot.regionOriginWorld.x,
                    slot.regionOriginWorld.y,
                    slot.regionOriginWorld.z);
            }
        }

        slot.pending = false;
        slot.fenceValue = 0;
        slot.bufferIndex = 0;
        slot.regionOriginWorld = glm::vec3(0.0f);
    }

    RefreshPersistentEditStats();
    uint32_t pendingSlots = 0;
    for (const auto& slot : m_brushEditFeedbackSlots) {
        if (slot.pending) {
            ++pendingSlots;
        }
    }
    m_streamingStats.gpuBrushFeedbackPending = pendingSlots;
}

void VoxelWorld::RecordPersistentBrushEdit(
    float localPositionX,
    float localPositionY,
    float localPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal)
{
    if (!IsUsingInfiniteChunks() || radius <= 0.0f) {
        return;
    }

    const int32_t radiusCeil = static_cast<int32_t>(std::ceil(radius)) + 2;
    const int32_t startX = std::max<int32_t>(0, FloorToInt(localPositionX) - radiusCeil);
    const int32_t startY = std::max<int32_t>(0, FloorToInt(localPositionY) - radiusCeil);
    const int32_t startZ = std::max<int32_t>(0, FloorToInt(localPositionZ) - radiusCeil);
    const int32_t endX = std::min<int32_t>(static_cast<int32_t>(m_config.gridSizeX), static_cast<int32_t>(std::ceil(localPositionX)) + radiusCeil + 1);
    const int32_t endY = std::min<int32_t>(static_cast<int32_t>(m_config.gridSizeY), static_cast<int32_t>(std::ceil(localPositionY)) + radiusCeil + 1);
    const int32_t endZ = std::min<int32_t>(static_cast<int32_t>(m_config.gridSizeZ), static_cast<int32_t>(std::ceil(localPositionZ)) + radiusCeil + 1);

    if (endX <= startX || endY <= startY || endZ <= startZ) {
        return;
    }

    uint32_t editedThisBrush = 0;
    uint32_t evaluatedThisBrush = 0;
    uint32_t rejectedThisBrush = 0;
    std::unordered_set<ChunkCoord> touchedChunks;
    const auto localToWorldVoxel = [this](int32_t x, int32_t y, int32_t z, int32_t& worldX, int32_t& worldY, int32_t& worldZ) -> bool {
        if (x < 0 || y < 0 || z < 0 ||
            x >= static_cast<int32_t>(m_config.gridSizeX) ||
            y >= static_cast<int32_t>(m_config.gridSizeY) ||
            z >= static_cast<int32_t>(m_config.gridSizeZ)) {
            return false;
        }
        const uint32_t slotX = static_cast<uint32_t>(x / CHUNK_SIZE_VOXELS);
        const uint32_t slotY = static_cast<uint32_t>(y / CHUNK_SIZE_VOXELS);
        const uint32_t slotZ = static_cast<uint32_t>(z / CHUNK_SIZE_VOXELS);
        const uint32_t slotIndex =
            slotX +
            slotY * RENDER_BUFFER_CHUNKS_X +
            slotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
        if (m_readBufferIndex >= m_chunkSlotWorldCoords.size() ||
            m_chunkSlotWorldCoords[m_readBufferIndex].empty()) {
            worldX = FloorToInt(m_regionOriginWorld.x) + x;
            worldY = FloorToInt(m_regionOriginWorld.y) + y;
            worldZ = FloorToInt(m_regionOriginWorld.z) + z;
            return true;
        }
        if (slotIndex >= m_chunkSlotWorldCoords[m_readBufferIndex].size()) {
            return false;
        }
        const ChunkCoord chunk = m_chunkSlotWorldCoords[m_readBufferIndex][slotIndex];
        if (chunk.x == INT32_MAX) {
            return false;
        }
        worldX = chunk.x * CHUNK_SIZE_VOXELS + (x % CHUNK_SIZE_VOXELS);
        worldY = chunk.y * CHUNK_SIZE_VOXELS + (y % CHUNK_SIZE_VOXELS);
        worldZ = chunk.z * CHUNK_SIZE_VOXELS + (z % CHUNK_SIZE_VOXELS);
        return true;
    };
    int32_t brushCenterWorldX = 0;
    int32_t brushCenterWorldY = 0;
    int32_t brushCenterWorldZ = 0;
    const bool brushCenterWorldValid = localToWorldVoxel(
        FloorToInt(localPositionX),
        FloorToInt(localPositionY),
        FloorToInt(localPositionZ),
        brushCenterWorldX,
        brushCenterWorldY,
        brushCenterWorldZ);
    const glm::vec3 brushCenterWorld = brushCenterWorldValid
        ? glm::vec3(static_cast<float>(brushCenterWorldX) + 0.5f,
                    static_cast<float>(brushCenterWorldY) + 0.5f,
                    static_cast<float>(brushCenterWorldZ) + 0.5f)
        : m_regionOriginWorld + glm::vec3(localPositionX, localPositionY, localPositionZ);
    const glm::vec3 hitNormal = glm::vec3(
        static_cast<float>(hitNormalX),
        static_cast<float>(hitNormalY),
        static_cast<float>(hitNormalZ));

    for (int32_t z = startZ; z < endZ; ++z) {
        for (int32_t y = startY; y < endY; ++y) {
            for (int32_t x = startX; x < endX; ++x) {
                ++evaluatedThisBrush;
                const float sdf = BrushSdf(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f,
                    static_cast<float>(z) + 0.5f,
                    localPositionX,
                    localPositionY,
                    localPositionZ,
                    radius,
                    shape);
                if (sdf > 0.5f) {
                    ++rejectedThisBrush;
                    continue;
                }

                int32_t worldX = 0;
                int32_t worldY = 0;
                int32_t worldZ = 0;
                if (!localToWorldVoxel(x, y, z, worldX, worldY, worldZ)) {
                    ++rejectedThisBrush;
                    continue;
                }
                if (worldY <= TERRAIN_MIN_Y + 5) {
                    ++rejectedThisBrush;
                    continue;
                }
                const uint32_t variant = HashVoxelVariant(worldX, worldY, worldZ, seed);
                uint32_t existingOverlayVoxel = 0;
                const bool hasExistingOverlay = TryGetPersistentVoxelEdit(worldX, worldY, worldZ, &existingOverlayVoxel);
                const uint32_t existingOverlayMaterial = hasExistingOverlay ? GetMaterialCPU(existingOverlayVoxel) : MAT_AIR_CPU;
                if (existingOverlayMaterial == MAT_BEDROCK_CPU) {
                    ++rejectedThisBrush;
                    continue;
                }

                if (strength < 1.0f && sdf > -0.5f) {
                    const float edgeFactor = std::clamp(1.0f - sdf / 0.5f, 0.0f, 1.0f);
                    const float probability = edgeFactor * strength;
                    if ((static_cast<float>(variant) / 255.0f) > probability) {
                        ++rejectedThisBrush;
                        continue;
                    }
                }

                if (hasHitNormal) {
                    const glm::vec3 voxelCenterWorld = glm::vec3(
                        static_cast<float>(worldX) + 0.5f,
                        static_cast<float>(worldY) + 0.5f,
                        static_cast<float>(worldZ) + 0.5f);
                    const float faceSide = glm::dot(voxelCenterWorld - brushCenterWorld, hitNormal);

                    if (mode == CPU_BRUSH_MODE_PAINT && faceSide < -0.35f) {
                        // Add/build mode only affects visible air. Without a CPU
                        // generated-terrain query, the raycast face is the best
                        // stable divider between exterior air and interior solid.
                        ++rejectedThisBrush;
                        continue;
                    }

                    if ((mode == CPU_BRUSH_MODE_ERASE || mode == CPU_BRUSH_MODE_REPLACE) && faceSide > 0.65f) {
                        // Erase/replace should target the solid side of the
                        // surface hit, not the empty air in front of it.
                        ++rejectedThisBrush;
                        continue;
                    }
                }

                uint32_t packedVoxel = 0;
                if (mode == CPU_BRUSH_MODE_ERASE) {
                    if (hasExistingOverlay && existingOverlayMaterial == MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(MAT_AIR_CPU, 0, 0, 0);
                } else if (mode == CPU_BRUSH_MODE_PAINT) {
                    if (hasExistingOverlay && existingOverlayMaterial != MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else if (mode == CPU_BRUSH_MODE_REPLACE) {
                    if (hasExistingOverlay && existingOverlayMaterial == MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else if (mode == CPU_BRUSH_MODE_FILL) {
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else {
                    ++rejectedThisBrush;
                    continue;
                }

                StorePersistentVoxelEdit(worldX, worldY, worldZ, packedVoxel);
                touchedChunks.insert(ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE));
                ++editedThisBrush;
            }
        }
    }

    UpdateTargetVoxelDebug(
        brushCenterWorldValid ? brushCenterWorldX : FloorToInt(m_regionOriginWorld.x + localPositionX),
        brushCenterWorldValid ? brushCenterWorldY : FloorToInt(m_regionOriginWorld.y + localPositionY),
        brushCenterWorldValid ? brushCenterWorldZ : FloorToInt(m_regionOriginWorld.z + localPositionZ),
        hasHitNormal ? hitNormalX : 0,
        hasHitNormal ? hitNormalY : 0,
        hasHitNormal ? hitNormalZ : 0);
    RefreshPersistentEditStats();
    m_streamingStats.brushVoxelsEvaluatedLastStroke = evaluatedThisBrush;
    m_streamingStats.brushVoxelsRejectedLastStroke = rejectedThisBrush;
    m_streamingStats.persistentEditsRecordedLastStroke = editedThisBrush;

    if (editedThisBrush > 0) {
        RequestActiveRegionRefresh(16);
        static uint32_t persistentBrushLogThrottle = 0;
        if ((++persistentBrushLogThrottle % 60u) == 1u) {
            spdlog::debug("Persistent brush edit recorded {} voxels across {} chunks (total edits={})",
                editedThisBrush,
                touchedChunks.size(),
                m_totalEditedVoxels);
        }
    }
}

void VoxelWorld::RecordPersistentWorldBrushEdit(
    float worldPositionX,
    float worldPositionY,
    float worldPositionZ,
    float radius,
    uint32_t material,
    uint32_t mode,
    uint32_t shape,
    float strength,
    uint32_t seed,
    int32_t hitNormalX,
    int32_t hitNormalY,
    int32_t hitNormalZ,
    bool hasHitNormal)
{
    if (!IsUsingInfiniteChunks() || radius <= 0.0f) {
        return;
    }

    const int32_t radiusCeil = static_cast<int32_t>(std::ceil(radius)) + 2;
    const int32_t centerX = FloorToInt(worldPositionX);
    const int32_t centerY = FloorToInt(worldPositionY);
    const int32_t centerZ = FloorToInt(worldPositionZ);
    const int32_t startX = centerX - radiusCeil;
    const int32_t startY = centerY - radiusCeil;
    const int32_t startZ = centerZ - radiusCeil;
    const int32_t endX = static_cast<int32_t>(std::ceil(worldPositionX)) + radiusCeil + 1;
    const int32_t endY = static_cast<int32_t>(std::ceil(worldPositionY)) + radiusCeil + 1;
    const int32_t endZ = static_cast<int32_t>(std::ceil(worldPositionZ)) + radiusCeil + 1;

    uint32_t editedThisBrush = 0;
    uint32_t evaluatedThisBrush = 0;
    uint32_t rejectedThisBrush = 0;
    std::unordered_set<ChunkCoord> touchedChunks;
    const glm::vec3 brushCenterWorld(worldPositionX, worldPositionY, worldPositionZ);
    const glm::vec3 hitNormal(
        static_cast<float>(hitNormalX),
        static_cast<float>(hitNormalY),
        static_cast<float>(hitNormalZ));

    for (int32_t z = startZ; z < endZ; ++z) {
        for (int32_t y = startY; y < endY; ++y) {
            for (int32_t x = startX; x < endX; ++x) {
                ++evaluatedThisBrush;
                const float sdf = BrushSdf(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f,
                    static_cast<float>(z) + 0.5f,
                    worldPositionX,
                    worldPositionY,
                    worldPositionZ,
                    radius,
                    shape);
                if (sdf > 0.5f) {
                    ++rejectedThisBrush;
                    continue;
                }
                if (y <= TERRAIN_MIN_Y + 5) {
                    ++rejectedThisBrush;
                    continue;
                }

                const uint32_t variant = HashVoxelVariant(x, y, z, seed);
                uint32_t existingOverlayVoxel = 0;
                const bool hasExistingOverlay = TryGetPersistentVoxelEdit(x, y, z, &existingOverlayVoxel);
                const uint32_t existingOverlayMaterial = hasExistingOverlay ? GetMaterialCPU(existingOverlayVoxel) : MAT_AIR_CPU;
                if (existingOverlayMaterial == MAT_BEDROCK_CPU) {
                    ++rejectedThisBrush;
                    continue;
                }

                if (strength < 1.0f && sdf > -0.5f) {
                    const float edgeFactor = std::clamp(1.0f - sdf / 0.5f, 0.0f, 1.0f);
                    const float probability = edgeFactor * strength;
                    if ((static_cast<float>(variant) / 255.0f) > probability) {
                        ++rejectedThisBrush;
                        continue;
                    }
                }

                if (hasHitNormal) {
                    const glm::vec3 voxelCenterWorld(
                        static_cast<float>(x) + 0.5f,
                        static_cast<float>(y) + 0.5f,
                        static_cast<float>(z) + 0.5f);
                    const float faceSide = glm::dot(voxelCenterWorld - brushCenterWorld, hitNormal);
                    if (mode == CPU_BRUSH_MODE_PAINT && faceSide < -0.35f) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    if ((mode == CPU_BRUSH_MODE_ERASE || mode == CPU_BRUSH_MODE_REPLACE) && faceSide > 0.65f) {
                        ++rejectedThisBrush;
                        continue;
                    }
                }

                uint32_t packedVoxel = 0;
                if (mode == CPU_BRUSH_MODE_ERASE) {
                    if (hasExistingOverlay && existingOverlayMaterial == MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(MAT_AIR_CPU, 0, 0, 0);
                } else if (mode == CPU_BRUSH_MODE_PAINT) {
                    if (hasExistingOverlay && existingOverlayMaterial != MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else if (mode == CPU_BRUSH_MODE_REPLACE) {
                    if (hasExistingOverlay && existingOverlayMaterial == MAT_AIR_CPU) {
                        ++rejectedThisBrush;
                        continue;
                    }
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else if (mode == CPU_BRUSH_MODE_FILL) {
                    packedVoxel = PackVoxelCPU(material, variant, 0, 0x80u);
                } else {
                    ++rejectedThisBrush;
                    continue;
                }

                StorePersistentVoxelEdit(x, y, z, packedVoxel);
                touchedChunks.insert(ChunkCoord::FromWorldPosition(x, y, z, INFINITE_CHUNK_SIZE));
                ++editedThisBrush;
            }
        }
    }

    UpdateTargetVoxelDebug(
        centerX,
        centerY,
        centerZ,
        hasHitNormal ? hitNormalX : 0,
        hasHitNormal ? hitNormalY : 0,
        hasHitNormal ? hitNormalZ : 0);
    RefreshPersistentEditStats();
    m_streamingStats.brushVoxelsEvaluatedLastStroke = evaluatedThisBrush;
    m_streamingStats.brushVoxelsRejectedLastStroke = rejectedThisBrush;
    m_streamingStats.persistentEditsRecordedLastStroke = editedThisBrush;

    if (editedThisBrush > 0) {
        RequestActiveRegionRefresh(16);
    }
}

bool VoxelWorld::RunEditOverlayCoordinateSelfTest() {
    VoxelWorld testWorld;
    const uint32_t testVoxel = PackVoxelCPU(3, 17, 0, 0);

    struct TestCoord {
        int32_t x;
        int32_t y;
        int32_t z;
        ChunkCoord expectedChunk;
        uint32_t expectedLocalX;
        uint32_t expectedLocalY;
        uint32_t expectedLocalZ;
    };

    const TestCoord tests[] = {
        {0, 0, 0, {0, 0, 0}, 0, 0, 0},
        {63, 63, 63, {0, 0, 0}, 63, 63, 63},
        {64, 64, 64, {1, 1, 1}, 0, 0, 0},
        {-1, -1, -1, {-1, -1, -1}, 63, 63, 63},
        {-64, -64, -64, {-1, -1, -1}, 0, 0, 0},
        {-65, -65, -65, {-2, -2, -2}, 63, 63, 63},
        {12, -129, 70, {0, -3, 1}, 12, 63, 6},
        {32, 664, -64, {0, 10, -1}, 32, 24, 0},
        {-129, -332, 128, {-3, -6, 2}, 63, 52, 0},
    };

    for (const auto& test : tests) {
        const ChunkCoord coord = ChunkCoord::FromWorldPosition(test.x, test.y, test.z, INFINITE_CHUNK_SIZE);
        if (coord != test.expectedChunk) {
            return false;
        }
        if (ChunkCoord::LocalCoord(test.x, INFINITE_CHUNK_SIZE) != test.expectedLocalX ||
            ChunkCoord::LocalCoord(test.y, INFINITE_CHUNK_SIZE) != test.expectedLocalY ||
            ChunkCoord::LocalCoord(test.z, INFINITE_CHUNK_SIZE) != test.expectedLocalZ) {
            return false;
        }

        testWorld.StorePersistentVoxelEdit(test.x, test.y, test.z, testVoxel);
        uint32_t storedVoxel = 0;
        if (!testWorld.TryGetPersistentVoxelEdit(test.x, test.y, test.z, &storedVoxel) || storedVoxel != testVoxel) {
            return false;
        }

        auto overlayIt = testWorld.m_editOverlays.find(coord);
        if (overlayIt == testWorld.m_editOverlays.end()) {
            return false;
        }

        const uint32_t expectedIndex = LocalVoxelIndex(test.expectedLocalX, test.expectedLocalY, test.expectedLocalZ);
        if (overlayIt->second.voxels.find(expectedIndex) == overlayIt->second.voxels.end()) {
            return false;
        }
    }

    VoxelWorld brushWorld;
    brushWorld.m_useInfiniteChunks = true;
    brushWorld.m_chunkManager = std::make_unique<InfiniteChunkManager>();
    brushWorld.m_config.gridSizeX = 192;
    brushWorld.m_config.gridSizeY = 128;
    brushWorld.m_config.gridSizeZ = 128;
    brushWorld.m_regionOriginWorld = glm::vec3(-64.0f, -64.0f, -64.0f);

    brushWorld.RecordPersistentBrushEdit(
        127.5f, 64.0f, 64.0f,
        2.25f,
        5,
        CPU_BRUSH_MODE_PAINT,
        CPU_BRUSH_SHAPE_SPHERE,
        1.0f,
        99,
        1, 0, 0,
        true);

    if (brushWorld.m_editOverlays.find(ChunkCoord{1, 0, 0}) == brushWorld.m_editOverlays.end()) {
        return false;
    }

    const uint32_t preExistingSolid = PackVoxelCPU(7, 1, 0, 0);
    brushWorld.StorePersistentVoxelEdit(64, 0, 0, preExistingSolid);
    brushWorld.RecordPersistentBrushEdit(
        128.0f, 64.0f, 64.0f,
        0.75f,
        8,
        CPU_BRUSH_MODE_PAINT,
        CPU_BRUSH_SHAPE_SPHERE,
        1.0f,
        100,
        1, 0, 0,
        true);
    uint32_t preservedVoxel = 0;
    if (!brushWorld.TryGetPersistentVoxelEdit(64, 0, 0, &preservedVoxel) || preservedVoxel != preExistingSolid) {
        return false;
    }

    brushWorld.RecordPersistentBrushEdit(
        1.0f, 1.0f, 1.0f,
        1.5f,
        0,
        CPU_BRUSH_MODE_ERASE,
        CPU_BRUSH_SHAPE_SPHERE,
        1.0f,
        101,
        0, 1, 0,
        true);
    if (brushWorld.m_editOverlays.find(ChunkCoord{-1, -1, -1}) == brushWorld.m_editOverlays.end()) {
        return false;
    }

    spdlog::info("Persistent edit overlay coordinate self-test passed ({} signed-coordinate cases)",
        static_cast<uint32_t>(sizeof(tests) / sizeof(tests[0])));
    return true;
}

Result<void> VoxelWorld::CreateChunkCopyPipeline(ID3D12Device* device) {
    // ===== COMPILE SHADER =====
    Graphics::ShaderCompiler compiler;
    auto initResult = compiler.Initialize();
    if (!initResult) {
        return Error("Failed to initialize shader compiler: {}", initResult.error());
    }

    std::filesystem::path shaderPath = "assets/shaders/Compute/CS_CopyChunkToBuffer.hlsl";
    auto compileResult = compiler.CompileComputeShader(shaderPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile CS_CopyChunkToBuffer.hlsl: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("CS_CopyChunkToBuffer.hlsl compilation failed: {}", compiledShader.errors);
    }

    // ===== CREATE ROOT SIGNATURE =====
    // Root parameter 0: root constants (CopyChunkConstants at b0)
    // Root parameter 1: SRV (ChunkVoxelInput at t0)
    // Root parameter 2: UAV (RenderBufferOutput at u0)
    // Root parameter 3: UAV (ChunkValidMask at u1)

    D3D12_ROOT_PARAMETER1 rootParams[4] = {};

    // Parameter 0: root constants
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: SRV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 2: UAV
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 3: valid-mask UAV
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[3].Descriptor.ShaderRegister = 1;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 4;
    rootSigDesc.Desc_1_1.pParameters = rootParams;
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
        return Error("Failed to serialize chunk copy root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_chunkCopyRootSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create chunk copy root signature");
    }

    // ===== CREATE PIPELINE STATE =====
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_chunkCopyRootSignature.Get();
    psoDesc.CS = compiledShader.GetBytecode();
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_chunkCopyPSO));
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy pipeline state");
    }

    m_chunkCopyPSO->SetName(L"CS_CopyChunkToBuffer_PSO");
    m_chunkCopyRootSignature->SetName(L"CS_CopyChunkToBuffer_RootSig");

    // ===== CREATE SHARED CONSTANT BUFFER =====
    // Persistent mapped buffer for copy parameters (32 bytes)
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(32);

    hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_chunkCopyConstantBuffer)
    );

    if (FAILED(hr)) {
        return Error("Failed to create chunk copy constant buffer");
    }

    m_chunkCopyConstantBuffer->SetName(L"ChunkCopyConstantBuffer");

    // Persistent mapping
    D3D12_RANGE readRange = {0, 0};
    hr = m_chunkCopyConstantBuffer->Map(0, &readRange, &m_chunkCopyConstantBufferMappedPtr);
    if (FAILED(hr)) {
        return Error("Failed to map chunk copy constant buffer");
    }

    // ===== RING BUFFER FIX: Create 3 command allocators for chunk copy =====
    for (uint32_t i = 0; i < NUM_COPY_BUFFERS; ++i) {
        hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_chunkCopyCmdAllocators[i])
        );
        if (FAILED(hr)) {
            return Error("Failed to create chunk copy command allocator {}", i);
        }
        m_copyAllocatorFenceValues[i] = 0;
    }

    // Create command list
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_chunkCopyCmdAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_chunkCopyCmdList)
    );
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy command list");
    }

    // Create GPU fence for synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_chunkCopyFence));
    if (FAILED(hr)) {
        return Error("Failed to create chunk copy fence");
    }

    m_chunkCopyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_chunkCopyFenceEvent) {
        return Error("Failed to create chunk copy fence event");
    }

    m_chunkCopyFenceValue = 0;
    m_currentCopyAllocatorIndex = 0;

    // Close command list (ready for Reset() in UpdateActiveRegion)
    m_chunkCopyCmdList->Close();

    spdlog::info("Chunk copy pipeline created successfully (ring buffer with {} allocators)", NUM_COPY_BUFFERS);
    return {};
}

Result<void> VoxelWorld::CreateEditApplyPipeline(ID3D12Device* device) {
    Graphics::ShaderCompiler compiler;
    auto initResult = compiler.Initialize();
    if (!initResult) {
        return Error("Failed to initialize shader compiler: {}", initResult.error());
    }

    std::filesystem::path shaderPath = "assets/shaders/Compute/CS_ApplyVoxelEdits.hlsl";
    auto compileResult = compiler.CompileComputeShader(shaderPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile CS_ApplyVoxelEdits.hlsl: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("CS_ApplyVoxelEdits.hlsl compilation failed: {}", compiledShader.errors);
    }

    D3D12_ROOT_PARAMETER1 rootParams[3] = {};

    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 3;
    rootSigDesc.Desc_1_1.pParameters = rootParams;
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
        return Error("Failed to serialize edit apply root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_editApplyRootSignature)
    );
    if (FAILED(hr)) {
        return Error("Failed to create edit apply root signature");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_editApplyRootSignature.Get();
    psoDesc.CS = compiledShader.GetBytecode();
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_editApplyPSO));
    if (FAILED(hr)) {
        return Error("Failed to create edit apply pipeline state");
    }

    m_editApplyPSO->SetName(L"CS_ApplyVoxelEdits_PSO");
    m_editApplyRootSignature->SetName(L"CS_ApplyVoxelEdits_RootSig");
    spdlog::info("Persistent edit apply pipeline created successfully");
    return {};
}

Result<void> VoxelWorld::EnsureEditUploadCapacity(ID3D12Device* device, uint32_t uploadSlot, uint32_t entryCount) {
    if (uploadSlot >= NUM_EDIT_UPLOAD_SLOTS) {
        return Error("Invalid persistent edit upload slot {}", uploadSlot);
    }

    if (entryCount == 0 || entryCount <= m_editUploadCapacities[uploadSlot]) {
        return {};
    }

    if (m_editUploadBuffers[uploadSlot] && m_editUploadMappedPtrs[uploadSlot]) {
        m_editUploadBuffers[uploadSlot]->Unmap(0, nullptr);
        m_editUploadMappedPtrs[uploadSlot] = nullptr;
    }
    m_editUploadBuffers[uploadSlot].Reset();

    uint32_t newCapacity = std::max<uint32_t>(1024, m_editUploadCapacities[uploadSlot]);
    while (newCapacity < entryCount) {
        newCapacity *= 2;
    }

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<UINT64>(newCapacity) * sizeof(GpuEditEntry));

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_editUploadBuffers[uploadSlot])
    );
    if (FAILED(hr)) {
        m_editUploadCapacities[uploadSlot] = 0;
        return Error("Failed to create persistent edit upload buffer");
    }

    m_editUploadBuffers[uploadSlot]->SetName(L"PersistentEditUploadBuffer");
    D3D12_RANGE readRange = {0, 0};
    hr = m_editUploadBuffers[uploadSlot]->Map(0, &readRange, &m_editUploadMappedPtrs[uploadSlot]);
    if (FAILED(hr)) {
        m_editUploadBuffers[uploadSlot].Reset();
        m_editUploadCapacities[uploadSlot] = 0;
        return Error("Failed to map persistent edit upload buffer");
    }

    m_editUploadCapacities[uploadSlot] = newCapacity;
    spdlog::debug("Persistent edit upload buffer slot {} capacity now {} entries",
        uploadSlot,
        m_editUploadCapacities[uploadSlot]);
    return {};
}

uint32_t VoxelWorld::ApplyPersistentEditsForCopiedChunks(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    uint32_t uploadSlot,
    const std::vector<CopiedChunkTarget>& copiedChunks,
    Graphics::GPUBuffer& targetBuffer)
{
    if (!device || !cmdList || copiedChunks.empty() || !m_editApplyPSO || !m_editApplyRootSignature) {
        return 0;
    }
    if (m_editOverlays.empty()) {
        return 0;
    }

    std::vector<GpuEditEntry> entries;
    uint32_t chunksWithEdits = 0;

    for (const auto& copied : copiedChunks) {
        auto overlayIt = m_editOverlays.find(copied.coord);
        if (overlayIt == m_editOverlays.end() || overlayIt->second.voxels.empty()) {
            continue;
        }

        bool chunkAddedEdits = false;
        for (const auto& [localIndex, packedVoxel] : overlayIt->second.voxels) {
            uint32_t localX = 0;
            uint32_t localY = 0;
            uint32_t localZ = 0;
            LocalVoxelFromIndex(localIndex, localX, localY, localZ);

            const int32_t targetX = copied.destX + static_cast<int32_t>(localX);
            const int32_t targetY = copied.destY + static_cast<int32_t>(localY);
            const int32_t targetZ = copied.destZ + static_cast<int32_t>(localZ);
            if (targetX < 0 || targetY < 0 || targetZ < 0 ||
                targetX >= static_cast<int32_t>(m_config.gridSizeX) ||
                targetY >= static_cast<int32_t>(m_config.gridSizeY) ||
                targetZ >= static_cast<int32_t>(m_config.gridSizeZ)) {
                continue;
            }

            const uint32_t targetIndex =
                static_cast<uint32_t>(targetX) +
                static_cast<uint32_t>(targetY) * m_config.gridSizeX +
                static_cast<uint32_t>(targetZ) * m_config.gridSizeX * m_config.gridSizeY;
            entries.push_back(GpuEditEntry{targetIndex, packedVoxel});
            chunkAddedEdits = true;
        }

        if (chunkAddedEdits) {
            ++chunksWithEdits;
        }
    }

    if (entries.empty()) {
        return 0;
    }

    auto capacityResult = EnsureEditUploadCapacity(device, uploadSlot, static_cast<uint32_t>(entries.size()));
    if (!capacityResult) {
        spdlog::error("Failed to grow persistent edit upload buffer: {}", capacityResult.error());
        return 0;
    }

    std::memcpy(m_editUploadMappedPtrs[uploadSlot], entries.data(), entries.size() * sizeof(GpuEditEntry));

    struct EditApplyConstants {
        uint32_t editCount;
        uint32_t padding0;
        uint32_t padding1;
        uint32_t padding2;
    };

    EditApplyConstants constants = {};
    constants.editCount = static_cast<uint32_t>(entries.size());

    cmdList->SetPipelineState(m_editApplyPSO.Get());
    cmdList->SetComputeRootSignature(m_editApplyRootSignature.Get());
    cmdList->SetComputeRoot32BitConstants(0, 4, &constants, 0);
    cmdList->SetComputeRootShaderResourceView(1, m_editUploadBuffers[uploadSlot]->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(2, targetBuffer.GetGPUVirtualAddress());
    cmdList->Dispatch((constants.editCount + 63) / 64, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(targetBuffer.GetResource());
    cmdList->ResourceBarrier(1, &uavBarrier);

    m_streamingStats.editsAppliedLastFrame += constants.editCount;
    m_streamingStats.chunksWithEditsAppliedLastFrame += chunksWithEdits;
    m_streamingStats.lastEditOverlayApplied = 1;

    static uint32_t editApplyLogThrottle = 0;
    if (++editApplyLogThrottle % 60 == 1) {
        spdlog::debug("Applied {} persistent voxel edits across {} copied chunks", entries.size(), chunksWithEdits);
    }
    return constants.editCount;
}

void VoxelWorld::SetChunkGenerationBudget(uint32_t chunksPerFrame) {
    if (m_chunkManager) {
        m_chunkManager->SetChunksPerFrame(chunksPerFrame);
    }
}

glm::vec3 VoxelWorld::WorldToRenderLocal(const glm::vec3& worldPosition) const {
    const int32_t worldX = FloorToInt(worldPosition.x);
    const int32_t worldY = FloorToInt(worldPosition.y);
    const int32_t worldZ = FloorToInt(worldPosition.z);
    const ChunkCoord chunk = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    const glm::ivec3 slot = RenderSlotCoordFromWorldChunk(chunk);
    return glm::vec3(
        static_cast<float>(slot.x * CHUNK_SIZE_VOXELS + FloorMod(worldX, CHUNK_SIZE_VOXELS)) + (worldPosition.x - std::floor(worldPosition.x)),
        static_cast<float>(slot.y * CHUNK_SIZE_VOXELS + FloorMod(worldY, CHUNK_SIZE_VOXELS)) + (worldPosition.y - std::floor(worldPosition.y)),
        static_cast<float>(slot.z * CHUNK_SIZE_VOXELS + FloorMod(worldZ, CHUNK_SIZE_VOXELS)) + (worldPosition.z - std::floor(worldPosition.z)));
}

bool VoxelWorld::RenderLocalToWorld(const glm::vec3& localPosition, glm::vec3& outWorldPosition) const {
    const int32_t localX = FloorToInt(localPosition.x);
    const int32_t localY = FloorToInt(localPosition.y);
    const int32_t localZ = FloorToInt(localPosition.z);
    if (localX < 0 || localY < 0 || localZ < 0 ||
        localX >= static_cast<int32_t>(m_config.gridSizeX) ||
        localY >= static_cast<int32_t>(m_config.gridSizeY) ||
        localZ >= static_cast<int32_t>(m_config.gridSizeZ)) {
        return false;
    }
    const uint32_t slotX = static_cast<uint32_t>(localX / CHUNK_SIZE_VOXELS);
    const uint32_t slotY = static_cast<uint32_t>(localY / CHUNK_SIZE_VOXELS);
    const uint32_t slotZ = static_cast<uint32_t>(localZ / CHUNK_SIZE_VOXELS);
    const uint32_t slotIndex =
        slotX +
        slotY * RENDER_BUFFER_CHUNKS_X +
        slotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
    if (m_readBufferIndex >= m_chunkSlotWorldCoords.size() ||
        slotIndex >= m_chunkSlotWorldCoords[m_readBufferIndex].size()) {
        return false;
    }
    const ChunkCoord chunk = m_chunkSlotWorldCoords[m_readBufferIndex][slotIndex];
    if (chunk.x == INT32_MAX) {
        return false;
    }
    outWorldPosition = glm::vec3(
        static_cast<float>(chunk.x * CHUNK_SIZE_VOXELS + (localX % CHUNK_SIZE_VOXELS)) + (localPosition.x - std::floor(localPosition.x)),
        static_cast<float>(chunk.y * CHUNK_SIZE_VOXELS + (localY % CHUNK_SIZE_VOXELS)) + (localPosition.y - std::floor(localPosition.y)),
        static_cast<float>(chunk.z * CHUNK_SIZE_VOXELS + (localZ % CHUNK_SIZE_VOXELS)) + (localPosition.z - std::floor(localPosition.z)));
    return true;
}

bool VoxelWorld::IsWorldChunkCachedForRead(const ChunkCoord& coord) const {
    if (!m_useInfiniteChunks) {
        return true;
    }

    const uint32_t slotIndex = RenderSlotIndexFromWorldChunk(coord);
    if (m_readBufferIndex >= m_chunkSlotWorldCoords.size() ||
        slotIndex >= m_chunkSlotWorldCoords[m_readBufferIndex].size()) {
        return false;
    }

    return m_chunkSlotWorldCoords[m_readBufferIndex][slotIndex] == coord;
}

bool VoxelWorld::IsWorldChunkCachedForWrite(const ChunkCoord& coord) const {
    if (!m_useInfiniteChunks) {
        return true;
    }

    const int writeBufferIndex = 1 - m_readBufferIndex;
    const uint32_t slotIndex = RenderSlotIndexFromWorldChunk(coord);
    if (writeBufferIndex < 0 ||
        writeBufferIndex >= static_cast<int>(m_chunkSlotWorldCoords.size()) ||
        slotIndex >= m_chunkSlotWorldCoords[writeBufferIndex].size()) {
        return false;
    }

    return m_chunkSlotWorldCoords[writeBufferIndex][slotIndex] == coord;
}

bool VoxelWorld::IsWorldChunkCachedForReadWrite(const ChunkCoord& coord) const {
    return IsWorldChunkCachedForRead(coord) && IsWorldChunkCachedForWrite(coord);
}

bool VoxelWorld::IsWorldVoxelCachedForRead(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    const ChunkCoord coord = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    return IsWorldChunkCachedForRead(coord);
}

bool VoxelWorld::IsWorldVoxelCachedForReadWrite(int32_t worldX, int32_t worldY, int32_t worldZ) const {
    const ChunkCoord coord = ChunkCoord::FromWorldPosition(worldX, worldY, worldZ, INFINITE_CHUNK_SIZE);
    return IsWorldChunkCachedForReadWrite(coord);
}

void VoxelWorld::RequestActiveRegionRefresh(uint32_t frames) {
    if (!m_useInfiniteChunks || frames == 0) {
        return;
    }
    m_forcedActiveRegionUpdateFrames = std::max(m_forcedActiveRegionUpdateFrames, frames);
}

bool VoxelWorld::EnsureWorldBrushVolumeCachedForReadWrite(float worldX, float worldY, float worldZ, float radius) {
    if (!m_useInfiniteChunks) {
        return true;
    }

    const int32_t radiusCeil = static_cast<int32_t>(std::ceil(std::max(radius, 0.0f))) + 2;
    const int32_t minX = static_cast<int32_t>(std::floor(worldX)) - radiusCeil;
    const int32_t minY = static_cast<int32_t>(std::floor(worldY)) - radiusCeil;
    const int32_t minZ = static_cast<int32_t>(std::floor(worldZ)) - radiusCeil;
    const int32_t maxX = static_cast<int32_t>(std::ceil(worldX)) + radiusCeil;
    const int32_t maxY = static_cast<int32_t>(std::ceil(worldY)) + radiusCeil;
    const int32_t maxZ = static_cast<int32_t>(std::ceil(worldZ)) + radiusCeil;

    const ChunkCoord minChunk = ChunkCoord::FromWorldPosition(minX, minY, minZ, INFINITE_CHUNK_SIZE);
    const ChunkCoord maxChunk = ChunkCoord::FromWorldPosition(maxX, maxY, maxZ, INFINITE_CHUNK_SIZE);

    bool fullyCached = true;
    uint32_t missingChunks = 0;
    for (int32_t z = minChunk.z; z <= maxChunk.z; ++z) {
        for (int32_t y = minChunk.y; y <= maxChunk.y; ++y) {
            for (int32_t x = minChunk.x; x <= maxChunk.x; ++x) {
                const ChunkCoord coord{x, y, z};
                if (IsWorldChunkCachedForReadWrite(coord)) {
                    continue;
                }

                fullyCached = false;
                ++missingChunks;
                if (m_chunkManager) {
                    const int32_t centerDx = x - ((minChunk.x + maxChunk.x) / 2);
                    const int32_t centerDy = y - ((minChunk.y + maxChunk.y) / 2);
                    const int32_t centerDz = z - ((minChunk.z + maxChunk.z) / 2);
                    const int32_t priority =
                        -3'200'000 +
                        (centerDx * centerDx + centerDz * centerDz) * 8 +
                        centerDy * centerDy * 2;
                    m_chunkManager->QueueUrgentChunk(coord, priority);
                }
            }
        }
    }

    if (!fullyCached) {
        RequestActiveRegionRefresh(16);
        static uint32_t missingBrushCacheLogThrottle = 0;
        if ((++missingBrushCacheLogThrottle % 60u) == 1u) {
            spdlog::debug("Brush volume waiting for {} chunk slots to converge around world ({:.1f},{:.1f},{:.1f})",
                missingChunks,
                worldX,
                worldY,
                worldZ);
        }
    }

    return fullyCached;
}

void VoxelWorld::PumpChunkGeneration(ID3D12Device* device, ID3D12CommandQueue* cmdQueue) {
    if (m_chunkManager) {
        m_chunkManager->GenerateQueuedChunks(device, cmdQueue);
    }
}

void VoxelWorld::CopyStatic2x2Chunks(ID3D12CommandQueue* cmdQueue) {
    if (!m_chunkManager || !cmdQueue) {
        return;
    }

    // Ensure copy pipeline is available
    if (!m_chunkCopyPSO || !m_chunkCopyRootSignature || !m_chunkCopyConstantBuffer || !m_chunkCopyFence) {
        spdlog::warn("CopyStatic2x2Chunks: chunk copy pipeline not initialized");
        return;
    }

    // ===== STEP 1: Acquire a copy allocator from the ring buffer =====
    uint32_t allocatorIndex = m_currentCopyAllocatorIndex;
    uint64_t allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
    uint32_t triesRemaining = NUM_COPY_BUFFERS;

    while (allocatorFenceValue > 0 &&
           m_chunkCopyFence->GetCompletedValue() < allocatorFenceValue &&
           triesRemaining > 0) {
        allocatorIndex = (allocatorIndex + 1) % NUM_COPY_BUFFERS;
        allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
        --triesRemaining;

        if (triesRemaining == 0) {
            static uint32_t skipFrameCount = 0;
            if (++skipFrameCount % 60 == 1) {  // Log once per second
                spdlog::warn("CopyStatic2x2Chunks: all {} copy allocators busy, skipping copy ({} times)",
                    NUM_COPY_BUFFERS, skipFrameCount);
            }
            return;
        }
    }

    HRESULT hr = m_chunkCopyCmdAllocators[allocatorIndex]->Reset();
    if (FAILED(hr)) {
        spdlog::error("CopyStatic2x2Chunks: failed to reset cmd allocator {} (HRESULT={:#x})",
            allocatorIndex, static_cast<uint32_t>(hr));
        return;
    }

    hr = m_chunkCopyCmdList->Reset(m_chunkCopyCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        spdlog::error("CopyStatic2x2Chunks: failed to reset cmd list (HRESULT={:#x})",
            static_cast<uint32_t>(hr));
        return;
    }

    m_chunkCopyCmdList->SetPipelineState(m_chunkCopyPSO.Get());
    m_chunkCopyCmdList->SetComputeRootSignature(m_chunkCopyRootSignature.Get());

    int32_t chunksCopied = 0;
    bool writeBufferTransitioned = false;
    bool writeMaskTransitioned = false;

    constexpr int32_t chunkSize = static_cast<int32_t>(INFINITE_CHUNK_SIZE);

    struct StaticEntry {
        ChunkCoord coord;
        int32_t destX;
        int32_t destY;
        int32_t destZ;
    };

    StaticEntry entries[4] = {
        { ChunkCoord{0, 0, 0}, 0,          0, 0          },
        { ChunkCoord{1, 0, 0}, chunkSize,  0, 0          },
        { ChunkCoord{0, 0, 1}, 0,          0, chunkSize  },
        { ChunkCoord{1, 0, 1}, chunkSize,  0, chunkSize  },
    };

    for (const auto& entry : entries) {
        Chunk* chunk = m_chunkManager->GetChunk(entry.coord);
        if (!chunk || !chunk->IsGenerated()) {
            continue;
        }

        // Bounds check against 256x128x256 grid
        if (entry.destX < 0 || entry.destY < 0 || entry.destZ < 0) {
            continue;
        }
        if (entry.destX + chunkSize > static_cast<int32_t>(m_config.gridSizeX) ||
            entry.destY + chunkSize > static_cast<int32_t>(m_config.gridSizeY) ||
            entry.destZ + chunkSize > static_cast<int32_t>(m_config.gridSizeZ)) {
            continue;
        }

        struct CopyChunkConstants {
            uint32_t destOffsetX;
            uint32_t destOffsetY;
            uint32_t destOffsetZ;
            uint32_t chunkSize;
            uint32_t destGridSizeX;
            uint32_t destGridSizeY;
            uint32_t destGridSizeZ;
            uint32_t validChunkIndex;
            uint32_t worldChunkX;
            uint32_t worldChunkY;
            uint32_t worldChunkZ;
            uint32_t paddingTag;
        };

        CopyChunkConstants constants;
        constants.destOffsetX = static_cast<uint32_t>(entry.destX);
        constants.destOffsetY = static_cast<uint32_t>(entry.destY);
        constants.destOffsetZ = static_cast<uint32_t>(entry.destZ);
        constants.chunkSize = INFINITE_CHUNK_SIZE;
        constants.destGridSizeX = m_config.gridSizeX;
        constants.destGridSizeY = m_config.gridSizeY;
        constants.destGridSizeZ = m_config.gridSizeZ;
        const uint32_t chunkSlotX = static_cast<uint32_t>(entry.destX / chunkSize);
        const uint32_t chunkSlotY = static_cast<uint32_t>(entry.destY / chunkSize);
        const uint32_t chunkSlotZ = static_cast<uint32_t>(entry.destZ / chunkSize);
        constants.validChunkIndex =
            chunkSlotX +
            chunkSlotY * RENDER_BUFFER_CHUNKS_X +
            chunkSlotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
        constants.worldChunkX = static_cast<uint32_t>(entry.coord.x);
        constants.worldChunkY = static_cast<uint32_t>(entry.coord.y);
        constants.worldChunkZ = static_cast<uint32_t>(entry.coord.z);
        constants.paddingTag = 0;

        if (!writeBufferTransitioned) {
            TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            writeBufferTransitioned = true;
        }
        if (!writeMaskTransitioned) {
            m_chunkValidMasks[1 - m_readBufferIndex].TransitionTo(
                m_chunkCopyCmdList.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            writeMaskTransitioned = true;
        }

        // Bind constants and resources. Root constants record this entry's
        // destination offset into the command stream; a shared upload CBV would
        // be overwritten by later entries before the GPU reads it.
        m_chunkCopyCmdList->SetComputeRoot32BitConstants(
            0,
            static_cast<UINT>(sizeof(CopyChunkConstants) / sizeof(uint32_t)),
            &constants,
            0
        );

        // Ensure chunk buffer is in SRV state before reading
        chunk->TransitionBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());
        m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetWriteBuffer().GetGPUVirtualAddress());
        m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(
            3,
            m_chunkValidMasks[1 - m_readBufferIndex].GetGPUVirtualAddress());

        // Dispatch 8x8x8 groups for 64^3 chunk
        m_chunkCopyCmdList->Dispatch(8, 8, 8);

        // UAV barrier between copies on WRITE buffer
        D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(GetWriteBuffer().GetResource());
        m_chunkCopyCmdList->ResourceBarrier(1, &uavBarrier);

        ++chunksCopied;
    }

    if (chunksCopied > 0) {
        // Transition WRITE buffer back to SRV for rendering after swap
        TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (writeMaskTransitioned) {
            m_chunkValidMasks[1 - m_readBufferIndex].TransitionTo(
                m_chunkCopyCmdList.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        m_chunkCopyCmdList->Close();
        ID3D12CommandList* lists[] = { m_chunkCopyCmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        m_chunkCopyFenceValue++;
        cmdQueue->Signal(m_chunkCopyFence.Get(), m_chunkCopyFenceValue);
        m_copyAllocatorFenceValues[allocatorIndex] = m_chunkCopyFenceValue;

        m_currentCopyAllocatorIndex = (m_currentCopyAllocatorIndex + 1) % NUM_COPY_BUFFERS;

        spdlog::debug("CopyStatic2x2Chunks: Copied {} chunks into WRITE buffer", chunksCopied);
    } else {
        // No chunks copied: clear fence tracking for this allocator and just close list
        m_copyAllocatorFenceValues[allocatorIndex] = 0;
        m_chunkCopyCmdList->Close();
    }
}

void VoxelWorld::UpdateActiveRegion(ID3D12Device* device, ID3D12CommandQueue* cmdQueue, bool clearRenderBuffers) {
    if (!m_chunkManager || !cmdQueue) {
        return;
    }

    // ===== STEP 1: Determine which chunks to copy into the moving render buffer =====
    // The buffer dimensions come from TerrainConstants.h and currently hold
    // Dense dev-window chunks. The final scalable renderer should promote
    // visible sparse bricks instead of expanding this dense cache.
    // We center on the camera's chunk and copy nearby chunks

    const auto& loadedChunks = m_chunkManager->GetLoadedChunks();
    if (loadedChunks.empty()) {
        m_streamingStats.copyBudget = m_maxChunkCopiesPerFrame;
        m_streamingStats.chunksCopiedLastFrame = 0;
        m_streamingStats.chunksSkippedLastFrame = 0;
        m_streamingStats.chunksNotGeneratedLastFrame = 0;
        m_streamingStats.chunksNotLoadedLastFrame = 0;
        m_streamingStats.urgentVisibleChunksQueuedLastFrame = 0;
        m_streamingStats.chunksCheckedLastFrame = 0;
        m_streamingStats.cachedReadChunks = 0;
        m_streamingStats.cachedWriteChunks = 0;
        m_streamingStats.expectedVisibleChunks =
            static_cast<uint32_t>((2 * RENDER_DISTANCE_HORIZONTAL + 1) *
                                  RENDER_BUFFER_CHUNKS_Y *
                                  (2 * RENDER_DISTANCE_HORIZONTAL + 1));
        m_streamingStats.editsAppliedLastFrame = 0;
        m_streamingStats.chunksWithEditsAppliedLastFrame = 0;
        m_streamingStats.lastEditOverlayApplied = 0;
        RefreshPersistentEditStats();
        return;  // No chunks loaded yet
    }

    const int32_t requestedCopyBudget = static_cast<int32_t>(m_maxChunkCopiesPerFrame);
    m_streamingStats.copyBudget = static_cast<uint32_t>(requestedCopyBudget);
    m_streamingStats.chunksCopiedLastFrame = 0;
    m_streamingStats.chunksSkippedLastFrame = 0;
    m_streamingStats.chunksNotGeneratedLastFrame = 0;
    m_streamingStats.chunksNotLoadedLastFrame = 0;
    m_streamingStats.urgentVisibleChunksQueuedLastFrame = 0;
    m_streamingStats.chunksCheckedLastFrame = 0;
    m_streamingStats.editsAppliedLastFrame = 0;
    m_streamingStats.chunksWithEditsAppliedLastFrame = 0;
    m_streamingStats.lastEditOverlayApplied = 0;
    RefreshPersistentEditStats();

    // Copy the moving chunk window into local render-buffer coordinates.

    // ===== STEP 2: Ring buffer - get next allocator =====
    uint32_t allocatorIndex = m_currentCopyAllocatorIndex;

    // FIX #5: Check if this allocator is still busy
    // IMPROVED: Try other allocators before giving up
    uint64_t allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
    uint32_t triesRemaining = NUM_COPY_BUFFERS;
    while (allocatorFenceValue > 0 && m_chunkCopyFence->GetCompletedValue() < allocatorFenceValue && triesRemaining > 0) {
        // This allocator is busy, try next one in ring buffer
        allocatorIndex = (allocatorIndex + 1) % NUM_COPY_BUFFERS;
        allocatorFenceValue = m_copyAllocatorFenceValues[allocatorIndex];
        triesRemaining--;

        if (triesRemaining == 0) {
            // All allocators are busy - skip this frame but log less frequently
            static uint32_t skipFrameCount = 0;
            if (++skipFrameCount % 60 == 1) {  // Log once per second
                spdlog::debug("All {} copy allocators busy, skipping chunk copy ({} times)",
                    NUM_COPY_BUFFERS, skipFrameCount);
            }
            return;  // Skip chunk copy this frame
        }
    }

    // NOW safe to reset this allocator (GPU has finished with it)
    // FIX #7: Check HRESULT - if Reset() fails, skip chunk copy this frame
    HRESULT hr = m_chunkCopyCmdAllocators[allocatorIndex]->Reset();
    if (FAILED(hr)) {
        spdlog::error("Failed to reset chunk copy cmd allocator {} (HRESULT={:#x}), skipping chunk copy",
            allocatorIndex, static_cast<uint32_t>(hr));
        return;  // Skip chunk copy this frame, will retry next frame
    }

    hr = m_chunkCopyCmdList->Reset(m_chunkCopyCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        spdlog::error("Failed to reset chunk copy cmd list (HRESULT={:#x}), skipping chunk copy",
            static_cast<uint32_t>(hr));
        return;  // Skip chunk copy this frame, will retry next frame
    }

    // FIX #6: Copy chunks to WRITE buffer (not READ)!
    // This prevents race condition where chunk copy and physics both write to same buffer
    // Architecture: UpdateChunks() writes NEW chunks -> WRITE, then physics simulates on READ -> WRITE
    //
    // NOTE: We defer the buffer transition until we know we'll actually copy chunks.
    // If we transition here but then copy 0 chunks, we'd update the CPU state tracking
    // without executing any GPU commands, causing state desync. See "deferred transition" below.

    ID3D12DescriptorHeap* descriptorHeaps[] = {
        m_heapManager ? m_heapManager->GetShaderVisibleCbvSrvUavHeap() : nullptr
    };
    if (descriptorHeaps[0]) {
        m_chunkCopyCmdList->SetDescriptorHeaps(1, descriptorHeaps);
    }

    // Set pipeline and root signature
    m_chunkCopyCmdList->SetPipelineState(m_chunkCopyPSO.Get());
    m_chunkCopyCmdList->SetComputeRootSignature(m_chunkCopyRootSignature.Get());

    // ===== STEP 3: Copy chunks within 9x2x9 grid centered on camera =====
    int32_t chunksCopied = 0;
    int32_t chunksSkipped = 0;
    int32_t chunksNotGenerated = 0;  // DEBUG: Track chunks that exist but aren't generated yet
    bool writeBufferTransitioned = false;  // Deferred transition flag
    bool readBufferTransitioned = false;   // Track READ buffer transition
    bool writeMaskTransitioned = false;
    bool readMaskTransitioned = false;
    std::vector<CopiedChunkTarget> copiedWriteChunks;
    std::vector<CopiedChunkTarget> copiedReadChunks;

    if (clearRenderBuffers) {
        // Recenter invalidates chunk slots, not the voxel memory itself. Clear
        // the small per-chunk valid masks instead of clearing multi-GB dense
        // buffers; shaders ignore stale voxel data until the chunk slot is
        // recopied and marked valid.
        m_chunkValidMasks[0].TransitionTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_chunkValidMasks[1].TransitionTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const UINT clearValues[4] = {0, 0, 0, 0};
        for (int bufferIndex = 0; bufferIndex < 2; ++bufferIndex) {
            if (bufferIndex < static_cast<int>(m_chunkSlotWorldCoords.size())) {
                std::fill(
                    m_chunkSlotWorldCoords[bufferIndex].begin(),
                    m_chunkSlotWorldCoords[bufferIndex].end(),
                    ChunkCoord(INT32_MAX, INT32_MAX, INT32_MAX));
            }
            m_chunkCopyCmdList->ClearUnorderedAccessViewUint(
                m_chunkValidMaskUAVs[bufferIndex].gpu,
                m_chunkValidMasks[bufferIndex].GetStagingUAV().cpu,
                m_chunkValidMasks[bufferIndex].GetResource(),
                clearValues,
                0,
                nullptr);

            D3D12_RESOURCE_BARRIER uavBarrier =
                CD3DX12_RESOURCE_BARRIER::UAV(m_chunkValidMasks[bufferIndex].GetResource());
            m_chunkCopyCmdList->ResourceBarrier(1, &uavBarrier);
        }

        readMaskTransitioned = true;
        writeMaskTransitioned = true;
        spdlog::debug("Cleared render-window chunk-valid masks before chunk refill");
    }

    const size_t expectedVisibleChunks =
        static_cast<size_t>((2 * RENDER_DISTANCE_HORIZONTAL + 1) *
                            RENDER_BUFFER_CHUNKS_Y *
                            (2 * RENDER_DISTANCE_HORIZONTAL + 1));
    const bool renderCachesIncomplete =
        m_copiedChunksPerBuffer[0].size() < expectedVisibleChunks ||
        m_copiedChunksPerBuffer[1].size() < expectedVisibleChunks;

    // Limit chunk copies per frame to prevent startup/recenter hitches. The
    // dense window is now hundreds of millions of voxels; a previous 512-chunk
    // burst could queue a very large GPU packet after a vertical recenter and
    // look like a crash/hang. Refill still gets a modest boost, but remains
    // bounded so the game can present intermediate frames.
    int32_t maxChunksPerFrame = requestedCopyBudget;
    const bool frameBudgetPressure = requestedCopyBudget < 32;
    if (m_framesAfterCacheInvalidation > 0 || !m_buffersStable || renderCachesIncomplete) {
        const int32_t refillMinBurst = frameBudgetPressure ? requestedCopyBudget : 40;
        const int32_t refillMaxBurst = frameBudgetPressure ? 32 : 64;
        maxChunksPerFrame = std::clamp<int32_t>(
            std::max<int32_t>(maxChunksPerFrame, refillMinBurst),
            1,
            refillMaxBurst);
    }
    if (clearRenderBuffers) {
        const int32_t recenterMinBurst = frameBudgetPressure ? requestedCopyBudget : 48;
        const int32_t recenterMaxBurst = frameBudgetPressure ? 48 : 64;
        maxChunksPerFrame = std::clamp<int32_t>(
            std::max<int32_t>(maxChunksPerFrame, recenterMinBurst),
            1,
            recenterMaxBurst);
    }

    int32_t chunksNotLoaded = 0;  // DEBUG: Count chunks not in loadedChunks map
    int32_t chunksChecked = 0;    // DEBUG: Total chunks checked
    int32_t chunksOutOfBounds = 0; // DEBUG: Chunks that failed bounds check

    int32_t renderChunkMinY = m_activeRegionCenter.y - RENDER_DISTANCE_VERTICAL_BELOW;
    int32_t renderChunkMaxY = m_activeRegionCenter.y + RENDER_DISTANCE_VERTICAL_ABOVE;

    struct CopyCandidate {
        int32_t dx;
        int32_t y;
        int32_t dz;
        int32_t priority;
    };

    std::vector<CopyCandidate> copyCandidates;
    copyCandidates.reserve(expectedVisibleChunks);

    for (int32_t dz = -RENDER_DISTANCE_HORIZONTAL; dz <= RENDER_DISTANCE_HORIZONTAL; ++dz) {
        for (int32_t y = renderChunkMinY; y <= renderChunkMaxY; ++y) {
            for (int32_t dx = -RENDER_DISTANCE_HORIZONTAL; dx <= RENDER_DISTANCE_HORIZONTAL; ++dx) {
                const int32_t dy = y - m_activeRegionCenter.y;
                const ChunkCoord worldChunk{
                    m_activeRegionCenter.x + dx,
                    y,
                    m_activeRegionCenter.z + dz
                };
                const int32_t cameraDx = worldChunk.x - m_lastRenderCameraChunk.x;
                const int32_t cameraDy = worldChunk.y - m_lastRenderCameraChunk.y;
                const int32_t cameraDz = worldChunk.z - m_lastRenderCameraChunk.z;
                const int32_t cameraDistance2 =
                    cameraDx * cameraDx * 8 +
                    cameraDz * cameraDz * 8 +
                    cameraDy * cameraDy * 2;
                const int32_t centerDistance2 = dx * dx + dz * dz + dy * dy;
                copyCandidates.push_back(CopyCandidate{
                    dx,
                    y,
                    dz,
                    cameraDistance2 * 16 + centerDistance2
                });
            }
        }
    }

    std::sort(copyCandidates.begin(), copyCandidates.end(),
        [](const CopyCandidate& a, const CopyCandidate& b) {
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            const int32_t aVertical = std::abs(a.y);
            const int32_t bVertical = std::abs(b.y);
            if (aVertical != bVertical) {
                return aVertical < bVertical;
            }
            if (a.dz != b.dz) {
                return a.dz < b.dz;
            }
            return a.dx < b.dx;
        });

    const size_t expectedChunks = expectedVisibleChunks;
    uint32_t urgentVisibleQueues = 0;
    constexpr uint32_t MAX_URGENT_VISIBLE_QUEUES_PER_FRAME = 256;

    // Visible pages are hard demand, not speculative background work. The
    // source cache can be "full" while still missing pages in the active
    // render window, especially after fast flight or a vertical recenter. Queue
    // those missing chunks explicitly before the copy budget early-outs.
    for (const CopyCandidate& candidate : copyCandidates) {
        if (urgentVisibleQueues >= MAX_URGENT_VISIBLE_QUEUES_PER_FRAME) {
            break;
        }

        const ChunkCoord chunkCoord = {
            m_activeRegionCenter.x + candidate.dx,
            candidate.y,
            m_activeRegionCenter.z + candidate.dz
        };

        if (loadedChunks.find(chunkCoord) == loadedChunks.end()) {
            const int32_t priority = -2'000'000 + candidate.priority;
            if (m_chunkManager->QueueUrgentChunk(chunkCoord, priority)) {
                ++urgentVisibleQueues;
            }
        }
    }

    for (const CopyCandidate& candidate : copyCandidates) {
                chunksChecked++;

                // CRITICAL FIX: Stop if we've copied enough this frame
                if (chunksCopied >= maxChunksPerFrame) {
                    goto done_copying;  // Break out of all loops
                }
                ChunkCoord chunkCoord = {
                    m_activeRegionCenter.x + candidate.dx,
                    candidate.y,
                    m_activeRegionCenter.z + candidate.dz
                };

                // Check if chunk is loaded
                auto it = loadedChunks.find(chunkCoord);
                if (it == loadedChunks.end()) {
                    chunksNotLoaded++;
                    continue;  // Chunk not loaded at all
                }
                if (!it->second->IsGenerated()) {
                    chunksNotGenerated++;
                    continue;  // Chunk exists but generation not complete yet
                }

                // Toroidal dense cache: physical slot is stable modulo world
                // chunk coordinate, so overlapping chunks survive recentering.
                const glm::ivec3 slotCoord = RenderSlotCoordFromWorldChunk(chunkCoord);
                const uint32_t slotIndex = RenderSlotIndexFromWorldChunk(chunkCoord);

                // Check if chunk is in BOTH buffers (convergent caching).
                // Trust the exact toroidal slot tag, not the legacy copied set:
                // a set entry can survive a recenter or overwrite while the
                // physical slot now contains another world chunk. Using the set
                // here caused permanent page misses because the copy pass kept
                // skipping chunks whose actual slot tag was stale.
                int writeBufferIndex = 1 - m_readBufferIndex;
                int readBufferIndex = m_readBufferIndex;
                bool inWriteBuffer =
                    slotIndex < m_chunkSlotWorldCoords[writeBufferIndex].size() &&
                    m_chunkSlotWorldCoords[writeBufferIndex][slotIndex] == chunkCoord;
                bool inReadBuffer =
                    slotIndex < m_chunkSlotWorldCoords[readBufferIndex].size() &&
                    m_chunkSlotWorldCoords[readBufferIndex][slotIndex] == chunkCoord;
                if (!inWriteBuffer) {
                    m_copiedChunksPerBuffer[writeBufferIndex].erase(chunkCoord);
                }
                if (!inReadBuffer) {
                    m_copiedChunksPerBuffer[readBufferIndex].erase(chunkCoord);
                }

                // Skip only if chunk is in BOTH buffers (fully converged)
                // This ensures both ping-pong buffers have the terrain data
                if (inWriteBuffer && inReadBuffer) {
                    chunksSkipped++;
                    continue;  // Already in both buffers, skip
                }

                // Chunk needs to be copied to at least one buffer

                Chunk* chunk = it->second;

                int32_t destX = slotCoord.x * CHUNK_SIZE_VOXELS;
                int32_t destY = slotCoord.y * CHUNK_SIZE_VOXELS;
                int32_t destZ = slotCoord.z * CHUNK_SIZE_VOXELS;

                // DIAGNOSTIC: Enable to debug chunk copy issues
                static int copyDebugCount = 0;
                if (copyDebugCount < 20) {
                    spdlog::debug("[CHUNK_COPY] Chunk coord [{},{},{}] dx={} dz={} -> buffer dest [{},{},{}]",
                        chunkCoord.x, chunkCoord.y, chunkCoord.z, candidate.dx, candidate.dz, destX, destY, destZ);
                    copyDebugCount++;
                }

                // Skip if out of bounds
                if (destX < 0 || destY < 0 || destZ < 0 ||
                    destX + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeX) ||
                    destY + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeY) ||
                    destZ + INFINITE_CHUNK_SIZE > static_cast<int32_t>(m_config.gridSizeZ)) {
                    chunksOutOfBounds++;
                    continue;
                }

                // ===== STEP 4: Update constant buffer =====
                struct CopyChunkConstants {
                    uint32_t destOffsetX;
                    uint32_t destOffsetY;
                    uint32_t destOffsetZ;
                    uint32_t chunkSize;
                    uint32_t destGridSizeX;
                    uint32_t destGridSizeY;
                    uint32_t destGridSizeZ;
                    uint32_t validChunkIndex;
                    uint32_t worldChunkX;
                    uint32_t worldChunkY;
                    uint32_t worldChunkZ;
                    uint32_t paddingTag;
                };

                CopyChunkConstants constants;
                constants.destOffsetX = static_cast<uint32_t>(destX);
                constants.destOffsetY = static_cast<uint32_t>(destY);
                constants.destOffsetZ = static_cast<uint32_t>(destZ);
                constants.chunkSize = INFINITE_CHUNK_SIZE;
                constants.destGridSizeX = m_config.gridSizeX;
                constants.destGridSizeY = m_config.gridSizeY;
                constants.destGridSizeZ = m_config.gridSizeZ;
                const uint32_t chunkSlotX = static_cast<uint32_t>(destX / CHUNK_SIZE_VOXELS);
                const uint32_t chunkSlotY = static_cast<uint32_t>(destY / CHUNK_SIZE_VOXELS);
                const uint32_t chunkSlotZ = static_cast<uint32_t>(destZ / CHUNK_SIZE_VOXELS);
                constants.validChunkIndex =
                    chunkSlotX +
                    chunkSlotY * RENDER_BUFFER_CHUNKS_X +
                    chunkSlotZ * RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y;
                constants.worldChunkX = static_cast<uint32_t>(chunkCoord.x);
                constants.worldChunkY = static_cast<uint32_t>(chunkCoord.y);
                constants.worldChunkZ = static_cast<uint32_t>(chunkCoord.z);
                constants.paddingTag = 0;

                // CRITICAL FIX: Transition chunk buffer to SRV state before reading!
                // After generation, chunks are left in UAV state. We MUST transition to
                // NON_PIXEL_SHADER_RESOURCE before using as SRV input for copy shader.
                chunk->TransitionBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                // Copy to READ buffer first. This is the buffer the renderer is
                // currently presenting, so center-out read fills remove the
                // fast-flight blank-window flash even if WRITE lags by a frame.
                if (!inReadBuffer) {
                    if (!readBufferTransitioned) {
                        TransitionReadBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        readBufferTransitioned = true;
                    }
                    if (!readMaskTransitioned) {
                        m_chunkValidMasks[readBufferIndex].TransitionTo(
                            m_chunkCopyCmdList.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        readMaskTransitioned = true;
                    }

                    m_chunkCopyCmdList->SetComputeRoot32BitConstants(
                        0,
                        static_cast<UINT>(sizeof(CopyChunkConstants) / sizeof(uint32_t)),
                        &constants,
                        0
                    );
                    m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetReadBuffer().GetGPUVirtualAddress());
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(
                        3,
                        m_chunkValidMasks[readBufferIndex].GetGPUVirtualAddress());
                    m_chunkCopyCmdList->Dispatch(8, 8, 8);

                    D3D12_RESOURCE_BARRIER uavBarriers[] = {
                        CD3DX12_RESOURCE_BARRIER::UAV(GetReadBuffer().GetResource()),
                        CD3DX12_RESOURCE_BARRIER::UAV(m_chunkValidMasks[readBufferIndex].GetResource())
                    };
                    m_chunkCopyCmdList->ResourceBarrier(2, uavBarriers);

                    if (constants.validChunkIndex < m_chunkSlotWorldCoords[readBufferIndex].size()) {
                        const ChunkCoord previous = m_chunkSlotWorldCoords[readBufferIndex][constants.validChunkIndex];
                        if (previous.x != INT32_MAX && previous != chunkCoord) {
                            m_copiedChunksPerBuffer[readBufferIndex].erase(previous);
                        }
                        m_chunkSlotWorldCoords[readBufferIndex][constants.validChunkIndex] = chunkCoord;
                    }
                    m_copiedChunksPerBuffer[readBufferIndex].insert(chunkCoord);
                    copiedReadChunks.push_back(CopiedChunkTarget{chunkCoord, destX, destY, destZ});
                    chunksCopied++;
                }

                if (chunksCopied >= maxChunksPerFrame) {
                    continue;
                }

                // Copy to WRITE buffer second so physics/brush ping-pong can
                // resume once the refill gate opens.
                if (!inWriteBuffer) {
                    if (!writeBufferTransitioned) {
                        TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        writeBufferTransitioned = true;
                    }
                    if (!writeMaskTransitioned) {
                        m_chunkValidMasks[writeBufferIndex].TransitionTo(
                            m_chunkCopyCmdList.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        writeMaskTransitioned = true;
                    }

                    m_chunkCopyCmdList->SetComputeRoot32BitConstants(
                        0,
                        static_cast<UINT>(sizeof(CopyChunkConstants) / sizeof(uint32_t)),
                        &constants,
                        0
                    );
                    m_chunkCopyCmdList->SetComputeRootShaderResourceView(1, chunk->GetVoxelBuffer().GetGPUVirtualAddress());
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(2, GetWriteBuffer().GetGPUVirtualAddress());
                    m_chunkCopyCmdList->SetComputeRootUnorderedAccessView(
                        3,
                        m_chunkValidMasks[writeBufferIndex].GetGPUVirtualAddress());
                    m_chunkCopyCmdList->Dispatch(8, 8, 8);

                    D3D12_RESOURCE_BARRIER uavBarriers[] = {
                        CD3DX12_RESOURCE_BARRIER::UAV(GetWriteBuffer().GetResource()),
                        CD3DX12_RESOURCE_BARRIER::UAV(m_chunkValidMasks[writeBufferIndex].GetResource())
                    };
                    m_chunkCopyCmdList->ResourceBarrier(2, uavBarriers);

                    if (constants.validChunkIndex < m_chunkSlotWorldCoords[writeBufferIndex].size()) {
                        const ChunkCoord previous = m_chunkSlotWorldCoords[writeBufferIndex][constants.validChunkIndex];
                        if (previous.x != INT32_MAX && previous != chunkCoord) {
                            m_copiedChunksPerBuffer[writeBufferIndex].erase(previous);
                        }
                        m_chunkSlotWorldCoords[writeBufferIndex][constants.validChunkIndex] = chunkCoord;
                    }
                    m_copiedChunksPerBuffer[writeBufferIndex].insert(chunkCoord);
                    copiedWriteChunks.push_back(CopiedChunkTarget{chunkCoord, destX, destY, destZ});
                    chunksCopied++;
                }
    }
    done_copying:  // Label for early exit when max chunks reached

    // DEBUG: Log copy statistics
    static int debugFrameCount = 0;
    debugFrameCount++;

    int writeBufferIndex = 1 - m_readBufferIndex;
    int readBufferIndex = m_readBufferIndex;
    const auto countExactVisibleCoverage = [&](int bufferIndex, uint32_t& mismatches) -> size_t {
        mismatches = 0;
        if (bufferIndex < 0 ||
            bufferIndex >= static_cast<int>(m_chunkSlotWorldCoords.size()) ||
            m_chunkSlotWorldCoords[bufferIndex].empty()) {
            mismatches = static_cast<uint32_t>(expectedChunks);
            return 0;
        }

        size_t coverage = 0;
        for (const CopyCandidate& candidate : copyCandidates) {
            const ChunkCoord chunkCoord{
                m_activeRegionCenter.x + candidate.dx,
                candidate.y,
                m_activeRegionCenter.z + candidate.dz
            };
            const uint32_t slotIndex = RenderSlotIndexFromWorldChunk(chunkCoord);
            if (slotIndex < m_chunkSlotWorldCoords[bufferIndex].size() &&
                m_chunkSlotWorldCoords[bufferIndex][slotIndex] == chunkCoord) {
                ++coverage;
            } else {
                ++mismatches;
            }
        }
        return coverage;
    };
    uint32_t readMismatches = 0;
    uint32_t writeMismatches = 0;
    size_t cachedInReadBuffer = countExactVisibleCoverage(readBufferIndex, readMismatches);
    size_t cachedInWriteBuffer = countExactVisibleCoverage(writeBufferIndex, writeMismatches);
    m_streamingStats.copyBudget = static_cast<uint32_t>(maxChunksPerFrame);
    m_streamingStats.chunksCopiedLastFrame = static_cast<uint32_t>(chunksCopied);
    m_streamingStats.chunksSkippedLastFrame = static_cast<uint32_t>(chunksSkipped);
    m_streamingStats.chunksNotGeneratedLastFrame = static_cast<uint32_t>(chunksNotGenerated);
    m_streamingStats.chunksNotLoadedLastFrame = static_cast<uint32_t>(chunksNotLoaded);
    m_streamingStats.urgentVisibleChunksQueuedLastFrame = urgentVisibleQueues;
    m_streamingStats.chunksCheckedLastFrame = static_cast<uint32_t>(chunksChecked);
    m_streamingStats.cachedReadChunks = static_cast<uint32_t>(cachedInReadBuffer);
    m_streamingStats.cachedWriteChunks = static_cast<uint32_t>(cachedInWriteBuffer);
    m_streamingStats.expectedVisibleChunks = static_cast<uint32_t>(expectedChunks);
    m_streamingStats.readSlotMismatches = readMismatches;
    m_streamingStats.writeSlotMismatches = writeMismatches;
    m_streamingStats.toroidalSlotCount =
        static_cast<uint32_t>(RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y * RENDER_BUFFER_CHUNKS_Z);

    if (!m_buffersStable) {
        if (cachedInReadBuffer >= expectedChunks && cachedInWriteBuffer >= expectedChunks) {
            m_buffersStable = true;
            m_framesAfterCacheInvalidation = 0;
            spdlog::debug("Render buffers fully converged after refill");
        } else if (m_framesAfterCacheInvalidation > 0) {
            --m_framesAfterCacheInvalidation;
        }
    }

    // Keep file logging low-volume in diagnostics mode. The overlay already
    // carries per-frame copy metrics; writing a debug line every copy pulse can
    // become part of the hitch being investigated.
    if (debugFrameCount % 60 == 1) {
        spdlog::debug("Chunks: {} copied | READ[{}]={} WRITE[{}]={} | notGen={} skip={}",
            chunksCopied, readBufferIndex, cachedInReadBuffer, writeBufferIndex, cachedInWriteBuffer,
            chunksNotGenerated, chunksSkipped);
    }

    // ===== STEP 6: Close and execute (ONLY if we copied something new) =====
    if (chunksCopied > 0 || clearRenderBuffers) {
        if (writeBufferTransitioned) {
            ApplyPersistentEditsForCopiedChunks(
                device,
                m_chunkCopyCmdList.Get(),
                allocatorIndex * 2u,
                copiedWriteChunks,
                GetWriteBuffer());
        }

        if (readBufferTransitioned) {
            ApplyPersistentEditsForCopiedChunks(
                device,
                m_chunkCopyCmdList.Get(),
                allocatorIndex * 2u + 1u,
                copiedReadChunks,
                GetReadBuffer());
        }

        // Transition WRITE buffer from UAV to SRV state
        if (writeBufferTransitioned) {
            TransitionWriteBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (writeMaskTransitioned) {
            m_chunkValidMasks[writeBufferIndex].TransitionTo(
                m_chunkCopyCmdList.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Transition READ buffer from UAV back to SRV state if we wrote to it
        if (readBufferTransitioned) {
            TransitionReadBufferTo(m_chunkCopyCmdList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (readMaskTransitioned) {
            m_chunkValidMasks[readBufferIndex].TransitionTo(
                m_chunkCopyCmdList.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        m_chunkCopyCmdList->Close();
        ID3D12CommandList* lists[] = { m_chunkCopyCmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);

        // ===== SIGNAL FENCE: Track when this copy operation completes =====
        m_chunkCopyFenceValue++;
        cmdQueue->Signal(m_chunkCopyFence.Get(), m_chunkCopyFenceValue);
        m_copyAllocatorFenceValues[allocatorIndex] = m_chunkCopyFenceValue;

        // NO BUFFER SWAP here! We write directly to WRITE buffer.
        // Chunks will appear after physics swaps buffers at end of frame.
        // This prevents race condition: UpdateChunks->WRITE, Physics reads READ writes WRITE, then swap.

        // PERFORMANCE FIX: Only log at debug level (info logs cause lag on Windows)
        // spdlog::debug("UpdateActiveRegion: Copied {} NEW chunks ({} skipped, {} not generated) to WRITE buffer",
        //     chunksCopied, chunksSkipped, chunksNotGenerated);

        // CRITICAL FIX: Only advance allocator index AFTER successful execution!
        // This ensures we don't skip allocators when no chunks are copied.
        m_currentCopyAllocatorIndex = (m_currentCopyAllocatorIndex + 1) % NUM_COPY_BUFFERS;
    } else {
        // CRITICAL FIX: If no chunks copied, we didn't execute or signal fence!
        // Clear the fence value for this allocator so next frame can use it.
        // We do NOT advance m_currentCopyAllocatorIndex - same allocator will be reused.
        m_copyAllocatorFenceValues[allocatorIndex] = 0;

        // No new chunks to copy - just close the command list without executing
        m_chunkCopyCmdList->Close();
        static uint32_t noCopyLogThrottle = 0;
        if ((chunksSkipped > 0 || chunksNotGenerated > 0 || chunksNotLoaded > 0 || chunksOutOfBounds > 0) &&
            (++noCopyLogThrottle % 120 == 1)) {
            spdlog::debug("UpdateActiveRegion: No chunks copied ({} skipped, {} not generated, {} not loaded, {} out of bounds)",
                chunksSkipped, chunksNotGenerated, chunksNotLoaded, chunksOutOfBounds);
        }
    }

    // NOTE: We do NOT wait for GPU completion here!
    // The ring buffer (3 allocators) ensures we won't reuse this allocator
    // for at least 2 more UpdateActiveRegion calls. GPU has plenty of time to complete.
}

} // namespace VENPOD::Simulation
