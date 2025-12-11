#include "InfiniteChunkManager.h"
#include "TerrainConstants.h"
#include <d3d12.h>
#include "../Graphics/RHI/d3dx12.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "../Graphics/RHI/DX12ComputePipeline.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_set>

namespace VENPOD::Simulation {

Result<void> InfiniteChunkManager::Initialize(
    ID3D12Device* device,
    Graphics::DescriptorHeapManager& heapManager,
    const InfiniteChunkConfig& config)
{
    if (!device) {
        return Error("InfiniteChunkManager::Initialize - device is null");
    }

    m_device = device;
    m_heapManager = &heapManager;
    m_config = config;

    // Create generation compute pipeline
    auto result = CreateGenerationPipeline(device);
    if (!result) {
        return Error("Failed to create generation pipeline: {}", result.error());
    }

    // ===== RING BUFFER FIX: Create 3 command allocators to prevent reuse while GPU executing =====
    for (uint32_t i = 0; i < NUM_FRAME_BUFFERS; ++i) {
        HRESULT hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_chunkCmdAllocators[i])
        );
        if (FAILED(hr)) {
            return Error("Failed to create chunk generation command allocator {}", i);
        }
        m_allocatorFenceValues[i] = 0;
    }

    // Create command list (will use allocators from ring buffer)
    HRESULT hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_chunkCmdAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_chunkCmdList)
    );
    if (FAILED(hr)) {
        return Error("Failed to create chunk generation command list");
    }

    // Close command list (ready for Reset() in GenerateNextChunk)
    m_chunkCmdList->Close();

    // ===== GPU FENCE: Create fence for tracking chunk generation completion =====
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_chunkFence));
    if (FAILED(hr)) {
        return Error("Failed to create chunk generation fence");
    }
    m_chunkFenceValue = 0;

    // Create fence event for CPU-GPU synchronization
    m_chunkFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_chunkFenceEvent) {
        return Error("Failed to create chunk generation fence event");
    }

    // FIX #10: Initialize allocator fence values to 0 to indicate "never used/ready"
    // The previous UINT64_MAX logic had a critical bug:
    //   - On Frame 0, allocatorFenceValue = UINT64_MAX
    //   - Fence check: if (UINT64_MAX != UINT64_MAX) → FALSE, but then...
    //   - Wait check: 0 < UINT64_MAX → TRUE, thinks allocator is BUSY!
    //   - Result: All 3 allocators fail busy check on Frame 0 → no chunks generate
    // Correct logic: Use 0 = "never used", check `if (value > 0)` to detect actual usage
    for (uint32_t i = 0; i < NUM_FRAME_BUFFERS; ++i) {
        m_allocatorFenceValues[i] = 0;  // 0 = ready for immediate use (never used)
    }

    spdlog::info("InfiniteChunkManager initialized - LOAD distance: {} chunks, UNLOAD distance: {} chunks (FIXED Y={},{} layers), seed: {}",
        m_config.loadDistanceHorizontal,
        m_config.unloadDistanceHorizontal,
        TERRAIN_CHUNK_MIN_Y, TERRAIN_CHUNK_MIN_Y + TERRAIN_NUM_CHUNKS_Y - 1,
        m_config.worldSeed);
    spdlog::info("Terrain generation: INDEPENDENT of camera Y - always at world Y={}-{} (chunks {},{})",
        TERRAIN_MIN_Y, TERRAIN_MIN_Y + (TERRAIN_NUM_CHUNKS_Y * CHUNK_SIZE_VOXELS) - 1,
        TERRAIN_CHUNK_MIN_Y, TERRAIN_CHUNK_MIN_Y + TERRAIN_NUM_CHUNKS_Y - 1);
    spdlog::info("Seamless streaming: chunks load at {} chunks, visible at {} chunks, unload at {} chunks",
        m_config.loadDistanceHorizontal, RENDER_DISTANCE_HORIZONTAL, m_config.unloadDistanceHorizontal);

    return {};
}

void InfiniteChunkManager::Shutdown() {
    // CRITICAL FIX #14: Wait for all GPU work to complete before shutdown
    // Without this, we can crash if chunks are still generating when app closes
    if (m_chunkFence && m_chunkFenceEvent) {
        uint64_t currentFenceValue = m_chunkFenceValue;
        uint64_t completedValue = m_chunkFence->GetCompletedValue();

        if (completedValue < currentFenceValue) {
            spdlog::info("Waiting for {} pending chunk generations to complete before shutdown...",
                m_chunkGenerationFences.size());

            HRESULT hr = m_chunkFence->SetEventOnCompletion(currentFenceValue, m_chunkFenceEvent);
            if (SUCCEEDED(hr)) {
                // Wait up to 5 seconds for GPU to finish
                DWORD waitResult = WaitForSingleObject(m_chunkFenceEvent, 5000);
                if (waitResult == WAIT_TIMEOUT) {
                    spdlog::error("Shutdown fence wait timeout - GPU may be hung!");
                } else if (waitResult == WAIT_OBJECT_0) {
                    spdlog::info("All chunk generations completed, proceeding with shutdown");
                }
            }
        }
    }

    // Free all loaded chunks
    for (auto& [coord, chunk] : m_loadedChunks) {
        if (chunk) {
            chunk->Shutdown();
            delete chunk;
        }
    }
    m_loadedChunks.clear();

    // Clear generation queue
    while (!m_generationQueue.empty()) {
        m_generationQueue.pop();
    }

    m_generationPSO.Reset();
    m_generationRootSignature.Reset();

    // Unmap and release shared constant buffer
    if (m_sharedConstantBuffer && m_sharedConstantBufferMappedPtr) {
        m_sharedConstantBuffer->Unmap(0, nullptr);
        m_sharedConstantBufferMappedPtr = nullptr;
    }
    m_sharedConstantBuffer.Reset();

    // Release fence resources
    if (m_chunkFenceEvent) {
        CloseHandle(m_chunkFenceEvent);
        m_chunkFenceEvent = nullptr;
    }
    m_chunkFence.Reset();

    // Release dedicated command list and ring buffer of allocators
    m_chunkCmdList.Reset();
    for (uint32_t i = 0; i < NUM_FRAME_BUFFERS; ++i) {
        m_chunkCmdAllocators[i].Reset();
    }

    m_device = nullptr;
    m_heapManager = nullptr;

    spdlog::info("InfiniteChunkManager shut down");
}

void InfiniteChunkManager::Update(
    ID3D12Device* device,
    ID3D12CommandQueue* cmdQueue,  // CHANGED: Need queue for immediate execution
    const glm::vec3& cameraWorldPos)
{
    if (!device || !cmdQueue || !m_heapManager) {
        return;
    }

    // CRITICAL FIX: Don't process chunks until fence is properly initialized
    if (!m_chunkFence) {
        spdlog::warn("InfiniteChunkManager::Update called before fence initialized");
        return;
    }

    // ===== STEP 0a: FIX #19 - Process deferred chunk deletes =====
    // Must happen FIRST to free chunks (buffers + descriptors) from old chunks before allocating new ones
    ProcessDeferredChunkDeletes();

    // ===== STEP 0b: FIX #1/#3 - Verify completed chunks and mark as Generated =====
    VerifyGeneratedChunks();

    // ===== STEP 1: Calculate camera's chunk coordinate (HORIZONTAL ONLY) =====
    // FIX: Terrain generation should be INDEPENDENT of camera Y position!
    // The terrain exists at Y=5 to Y=60, which spans chunks Y=0 and Y=64.
    // We should ALWAYS load those chunks, regardless of camera altitude.
    // Only use camera X/Z to determine horizontal chunk position.
    ChunkCoord cameraChunk = ChunkCoord::FromWorldPosition(
        static_cast<int32_t>(cameraWorldPos.x),
        0,  // FIX: Always use Y=0 as reference (terrain is at Y=0-64 chunk range)
        static_cast<int32_t>(cameraWorldPos.z),
        INFINITE_CHUNK_SIZE
    );

    // PRIORITY 2: Store camera chunk for external access (chunk scan optimization)
    m_cameraChunk = cameraChunk;

    // Only update if camera moved to different chunk (avoid redundant work)
    if (cameraChunk == m_lastCameraChunk) {
        // CRITICAL FIX: Continue generating queued chunks even when stationary
        // The previous logic had a deadlock: after 3 chunks, it would return if queue wasn't empty,
        // causing 20+ chunks to remain stuck in queue forever → system freeze
        m_stationaryFrameCount++;

        // If we have queued chunks, keep generating them (1 per frame is safe)
        if (!m_generationQueue.empty()) {
            GenerateNextChunk(device, cmdQueue);
            return;  // Continue next frame
        }

        // Queue is empty - check if we should re-queue chunks
        // Only re-queue every 10 frames when stationary to avoid redundant work
        if (m_stationaryFrameCount > 10) {
            m_stationaryFrameCount = 0;  // Reset counter
            // Fall through to QueueChunksAroundCamera() below
        } else {
            return;  // Queue empty, wait before re-queueing
        }
    } else {
        // Camera moved - reset stationary counter and re-queue immediately
        m_stationaryFrameCount = 0;
    }

    m_lastCameraChunk = cameraChunk;

    // Log only when camera chunk changes (not every frame)
    static ChunkCoord lastLoggedChunk = {INT32_MAX, INT32_MAX, INT32_MAX};
    if (cameraChunk != lastLoggedChunk) {
        spdlog::debug("Camera chunk: [{},{},{}] - world pos: ({:.1f},{:.1f},{:.1f})",
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            cameraWorldPos.x, cameraWorldPos.y, cameraWorldPos.z);
        lastLoggedChunk = cameraChunk;
    }

    // ===== STEP 2: Queue chunks within cylindrical render distance =====
    auto queueResult = QueueChunksAroundCamera(cameraChunk);
    if (!queueResult) {
        spdlog::warn("Failed to queue chunks: {}", queueResult.error());
    }

    // ===== STEP 3: Generate ONE chunk per frame to prevent GPU flooding =====
    // CRITICAL FIX: Only generate 1 chunk per frame to prevent:
    // 1. GPU command queue flooding (was causing system crash)
    // 2. VRAM exhaustion from too many simultaneous allocations
    // 3. Ring buffer contention (3 allocators can't handle 1445 chunks)
    if (!m_generationQueue.empty()) {
        GenerateNextChunk(device, cmdQueue);
    }

    // ===== STEP 4: Unload distant chunks =====
    UnloadDistantChunks(cameraChunk);

    // Log only once per second to avoid spam
    static int chunkStatsThrottle = 0;
    if (++chunkStatsThrottle % 60 == 1) {
        spdlog::debug("Chunks loaded: {}, queued: {}",
            m_loadedChunks.size(),
            m_generationQueue.size());
    }
}

Chunk* InfiniteChunkManager::GetChunk(const ChunkCoord& coord) {
    auto it = m_loadedChunks.find(coord);
    return (it != m_loadedChunks.end()) ? it->second : nullptr;
}

const Chunk* InfiniteChunkManager::GetChunk(const ChunkCoord& coord) const {
    auto it = m_loadedChunks.find(coord);
    return (it != m_loadedChunks.end()) ? it->second : nullptr;
}

Result<void> InfiniteChunkManager::ForceGenerateChunk(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const ChunkCoord& coord)
{
    if (!device || !cmdList) {
        return Error("ForceGenerateChunk - null parameters");
    }

    // Check if already loaded
    if (GetChunk(coord) != nullptr) {
        return {};  // Already generated
    }

    // Create chunk
    Chunk* chunk = new Chunk();
    auto result = chunk->Initialize(device, *m_heapManager, coord, "InfiniteChunk");
    if (!result) {
        delete chunk;
        return Error("Failed to initialize chunk: {}", result.error());
    }

    // Generate chunk using shared constant buffer
    result = chunk->Generate(
        device,
        cmdList,
        m_generationPSO.Get(),
        m_generationRootSignature.Get(),
        m_sharedConstantBuffer.Get(),
        m_sharedConstantBufferMappedPtr,
        m_config.worldSeed
    );

    if (!result) {
        chunk->Shutdown();
        delete chunk;
        return Error("Failed to generate chunk: {}", result.error());
    }

    // SYNC FIX: For force generation (testing), mark as Generated immediately
    // since the test expects synchronous generation and will execute+wait itself
    chunk->MarkGenerated();

    // Add to loaded chunks
    m_loadedChunks[coord] = chunk;

    spdlog::info("Force-generated chunk [{},{},{}]", coord.x, coord.y, coord.z);
    return {};
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

Result<void> InfiniteChunkManager::QueueChunksAroundCamera(const ChunkCoord& cameraChunk) {
    // CRITICAL FIX: Limit queue size to prevent GPU command flooding and VRAM exhaustion
    // This was causing the system crash on launch!
    if (m_generationQueue.size() >= m_config.maxQueuedChunks) {
        spdlog::debug("Queue full ({}/{}), skipping new chunk queueing",
            m_generationQueue.size(), m_config.maxQueuedChunks);
        return {};  // Queue is full, wait for existing chunks to generate
    }

    // Use unordered_set to avoid duplicate queue entries
    std::unordered_set<ChunkCoord> chunksToLoad;

    // FIX #1: Calculate remaining queue capacity BEFORE loop
    size_t remainingCapacity = m_config.maxQueuedChunks - m_generationQueue.size();

    // ===== FIXED VERTICAL LAYERS + HORIZONTAL LOADING =====
    // FIX: Terrain is at Y=TERRAIN_MIN_Y to Y=TERRAIN_MAX_Y, which spans chunks Y=0 and Y=1
    // We ALWAYS load these TERRAIN_NUM_CHUNKS_Y vertical layers, regardless of camera altitude.
    // Only horizontal (X/Z) position varies based on camera.
    //
    // SEAMLESS STREAMING: Use loadDistanceHorizontal (larger than render distance)
    // This loads chunks BEFORE they become visible, so player never sees pop-in.
    for (int32_t dy = TERRAIN_CHUNK_MIN_Y; dy < TERRAIN_CHUNK_MIN_Y + TERRAIN_NUM_CHUNKS_Y; ++dy) {
        for (int32_t dx = -m_config.loadDistanceHorizontal; dx <= m_config.loadDistanceHorizontal; ++dx) {
            for (int32_t dz = -m_config.loadDistanceHorizontal; dz <= m_config.loadDistanceHorizontal; ++dz) {
                // FIX #1: Stop if we've already collected enough chunks
                if (chunksToLoad.size() >= remainingCapacity) {
                    goto done_collecting;  // Break all loops
                }

                // FIX: Use SQUARE pattern to match VoxelWorld renderer expectations
                // The renderer scans a square grid (25x25), so we must load chunks in a square pattern
                // Previously used circular radius which skipped corners → 124 missing chunks → holes
                // Now using Chebyshev distance (max of abs(dx), abs(dz)) for square coverage
                //
                // Example with renderDistance=12:
                // - Square: loads all chunks where |dx| <= 12 AND |dz| <= 12 → 25×25 = 625 chunks
                // - Circle: loads chunks where dx²+dz² <= 144 → ~501 chunks (misses 124 corners!)
                //
                // No distance check needed - the loop bounds already define the square:
                // for dx in [-renderDistance, +renderDistance]
                // for dz in [-renderDistance, +renderDistance]
                // All chunks within this square are needed by the renderer

                ChunkCoord coord = {
                    cameraChunk.x + dx,
                    dy,  // FIX: Absolute Y coordinate (0 or 1), not relative to camera
                    cameraChunk.z + dz
                };

                // Check if already loaded OR already queued for generation
                if (m_loadedChunks.find(coord) != m_loadedChunks.end()) {
                    continue;  // Already loaded
                }

                // CRITICAL FIX: Check if already pending generation (prevents duplicate queuing)
                if (m_chunkGenerationFences.find(coord) != m_chunkGenerationFences.end()) {
                    continue;  // Already queued/generating
                }

                chunksToLoad.insert(coord);
            }
        }
    }
    done_collecting:

    // Add new chunks to generation queue (guaranteed to not exceed maxQueuedChunks)
    for (const auto& coord : chunksToLoad) {
        m_generationQueue.push(coord);
    }

    if (chunksToLoad.size() > 0) {
        spdlog::debug("Queued {} new chunks for generation at Y=0,1 (queue size: {}/{}) - load distance: {} chunks",
            chunksToLoad.size(), m_generationQueue.size(), m_config.maxQueuedChunks,
            m_config.loadDistanceHorizontal);

        // Log first few chunks being queued for verification
        static bool firstLog = true;
        if (firstLog && chunksToLoad.size() > 0) {
            spdlog::info("Seamless streaming: loading chunks up to {} chunks away (visible at {} chunks)",
                m_config.loadDistanceHorizontal, RENDER_DISTANCE_HORIZONTAL);
            spdlog::info("First chunks queued (all at Y=0 or Y=1):");
            int count = 0;
            for (const auto& coord : chunksToLoad) {
                if (count++ < 5) {
                    spdlog::info("  Chunk [{},{},{}]", coord.x, coord.y, coord.z);
                }
            }
            firstLog = false;
        }
    }
    return {};
}

Result<void> InfiniteChunkManager::GenerateNextChunk(
    ID3D12Device* device,
    ID3D12CommandQueue* cmdQueue)  // CHANGED: Use queue for immediate execution
{
    if (m_generationQueue.empty()) {
        return {};
    }

    ChunkCoord coord = m_generationQueue.front();
    m_generationQueue.pop();

    // Skip if already loaded (could have been queued multiple times)
    if (m_loadedChunks.find(coord) != m_loadedChunks.end()) {
        return {};
    }

    // Skip if already pending generation (prevents double-generation)
    if (m_chunkGenerationFences.find(coord) != m_chunkGenerationFences.end()) {
        spdlog::debug("Chunk [{},{},{}] already pending generation, skipping",
            coord.x, coord.y, coord.z);
        return {};
    }

    // ===== RING BUFFER FIX: Get next allocator from ring buffer =====
    uint32_t allocatorIndex = m_currentAllocatorIndex;

    // CRITICAL FIX: Check if this allocator is still busy - if so, SKIP this frame
    // instead of waiting (prevents CPU stalls on startup when generating many chunks)
    uint64_t allocatorFenceValue = m_allocatorFenceValues[allocatorIndex];
    // FIX #10: 0 means "never used/ready", only check fence if allocator was actually used
    if (allocatorFenceValue > 0 && m_chunkFence->GetCompletedValue() < allocatorFenceValue) {
        // GPU still using this allocator - check if we should re-queue or drop
        // FIX: Track re-queue attempts to prevent infinite loops
        uint32_t requeueCount = ++m_chunkRequeueCount[coord];

        // If this chunk has been re-queued too many times (50+ frames), drop it temporarily
        // It will be re-queued naturally when camera moves or queue empties
        constexpr uint32_t MAX_REQUEUE_ATTEMPTS = 50;
        if (requeueCount > MAX_REQUEUE_ATTEMPTS) {
            spdlog::warn("Chunk [{},{},{}] exceeded {} re-queue attempts, dropping (will retry later)",
                coord.x, coord.y, coord.z, MAX_REQUEUE_ATTEMPTS);
            m_chunkRequeueCount.erase(coord);  // Reset counter
            return {};  // Drop this attempt, don't re-queue
        }

        // Re-queue for next frame
        m_generationQueue.push(coord);
        spdlog::debug("Allocator {} busy (fence {} > completed {}), re-queuing chunk [{},{},{}] (attempt {})",
            allocatorIndex, allocatorFenceValue, m_chunkFence->GetCompletedValue(),
            coord.x, coord.y, coord.z, requeueCount);
        return {};  // Skip chunk generation this frame, will retry next frame
    }

    // REMOVED BAD CODE: Don't reset to GetCompletedValue() here!
    // We'll set it to the NEW fence value after we actually use this allocator (line 473)

    // ===== OPTIMIZATION: Check descriptor heap capacity BEFORE resetting allocator =====
    // This avoids wasting a GPU round-trip if we can't allocate descriptors anyway
    const uint32_t DESCRIPTORS_PER_CHUNK = 2;
    const uint32_t SAFETY_MARGIN = 10;  // Reserve some for frame resources

    // CRITICAL: Check heap manager is valid before accessing
    if (!m_heapManager) {
        spdlog::critical("FATAL: HeapManager is nullptr in GenerateNextChunk!");
        return Error("HeapManager is null");
    }

    auto* heap = m_heapManager->GetShaderVisibleCbvSrvUavHeap();
    if (!heap) {
        spdlog::critical("FATAL: Shader-visible heap is nullptr!");
        return Error("Descriptor heap is null");
    }

    uint32_t currentDescriptors = m_heapManager->GetShaderVisibleCbvSrvUavAllocatedCount();
    uint32_t maxDescriptors = heap->GetDesc().NumDescriptors;

    // spdlog::debug("HEAP CHECK: {}/{} descriptors allocated", currentDescriptors, maxDescriptors);

    if (currentDescriptors + DESCRIPTORS_PER_CHUNK + SAFETY_MARGIN > maxDescriptors) {
        // CRITICAL FIX: Apply re-queue limit to prevent infinite loops when heap is full
        // Without this, chunks bounce in queue forever without the allocator busy check
        uint32_t requeueCount = ++m_chunkRequeueCount[coord];

        constexpr uint32_t MAX_REQUEUE_ATTEMPTS = 50;
        if (requeueCount > MAX_REQUEUE_ATTEMPTS) {
            spdlog::warn("Chunk [{},{},{}] dropped due to descriptor exhaustion after {} attempts (heap: {}/{})",
                coord.x, coord.y, coord.z, MAX_REQUEUE_ATTEMPTS, currentDescriptors, maxDescriptors);
            m_chunkRequeueCount.erase(coord);  // Reset counter
            return {};  // Drop permanently - will retry when heap has space
        }

        // Heap is full - re-queue chunk without touching allocator
        spdlog::debug("Descriptor heap full ({}/{} descriptors), deferring chunk [{},{},{}] (attempt {})",
            currentDescriptors, maxDescriptors, coord.x, coord.y, coord.z, requeueCount);
        m_generationQueue.push(coord);  // Re-queue for later
        return {};  // Not an error, just deferring
    }

    // CRITICAL FIX: DO NOT advance allocator index yet!
    // We must only advance AFTER successful chunk creation, otherwise we consume
    // an allocator slot without creating a chunk → allocator starvation

    // NOW safe to reset this allocator (GPU has finished with it)
    // FIX #7: Check HRESULT - if Reset() fails, re-queue chunk and skip this frame
    // spdlog::debug("SYNC CHECK: About to reset allocator {} (fence value: {}, GPU completed: {})",
    //     allocatorIndex, allocatorFenceValue, m_chunkFence->GetCompletedValue());

    // CRITICAL ASSERTION: Verify allocator is not nullptr before reset
    if (!m_chunkCmdAllocators[allocatorIndex]) {
        spdlog::critical("FATAL: Command allocator {} is nullptr!", allocatorIndex);
        return Error("Command allocator is null");
    }

    HRESULT hr = m_chunkCmdAllocators[allocatorIndex]->Reset();
    if (FAILED(hr)) {
        m_generationQueue.push(coord);  // Re-queue for retry
        spdlog::error("Failed to reset chunk cmd allocator {} (HRESULT={:#x}), re-queuing chunk [{},{},{}]",
            allocatorIndex, static_cast<uint32_t>(hr), coord.x, coord.y, coord.z);
        return Error("Command allocator Reset() failed");
    }
    // spdlog::debug("SYNC CHECK: Allocator {} reset successful", allocatorIndex);

    hr = m_chunkCmdList->Reset(m_chunkCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        m_generationQueue.push(coord);  // Re-queue for retry
        spdlog::error("Failed to reset chunk cmd list (HRESULT={:#x}), re-queuing chunk [{},{},{}]",
            static_cast<uint32_t>(hr), coord.x, coord.y, coord.z);
        return Error("Command list Reset() failed");
    }

    // ===== CREATE CHUNK =====
    Chunk* chunk = new Chunk();
    auto result = chunk->Initialize(device, *m_heapManager, coord, "InfiniteChunk");
    if (!result) {
        delete chunk;
        return Error("Failed to initialize chunk [{},{},{}]: {}",
            coord.x, coord.y, coord.z, result.error());
    }

    // ===== GENERATE CHUNK (using shared constant buffer + dedicated cmdList) =====
    result = chunk->Generate(
        device,
        m_chunkCmdList.Get(),  // Use dedicated command list
        m_generationPSO.Get(),
        m_generationRootSignature.Get(),
        m_sharedConstantBuffer.Get(),
        m_sharedConstantBufferMappedPtr,
        m_config.worldSeed
    );

    if (!result) {
        chunk->Shutdown();
        delete chunk;
        return Error("Failed to generate chunk [{},{},{}]: {}",
            coord.x, coord.y, coord.z, result.error());
    }

    // ===== EXECUTE CHUNK GENERATION ASYNCHRONOUSLY =====
    m_chunkCmdList->Close();
    ID3D12CommandList* lists[] = { m_chunkCmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);

    // ===== SIGNAL FENCE: Track when this chunk generation completes =====
    m_chunkFenceValue++;
    cmdQueue->Signal(m_chunkFence.Get(), m_chunkFenceValue);
    m_allocatorFenceValues[allocatorIndex] = m_chunkFenceValue;

    // ===== ADD TO LOADED CHUNKS MAP IMMEDIATELY =====
    // We add the chunk to the map now, but it's still in GenerationSubmitted state.
    // Update() will call VerifyGeneratedChunks() each frame to poll the fence and
    // mark chunks as Generated when the GPU finishes.
    m_loadedChunks[coord] = chunk;

    // Store the fence value for this specific chunk so we can poll it later
    m_chunkGenerationFences[coord] = m_chunkFenceValue;

    // Clear re-queue counter on successful generation
    m_chunkRequeueCount.erase(coord);

    // CRITICAL FIX: Move to next allocator ONLY after successful chunk generation
    // This prevents allocator starvation if any step above fails
    m_currentAllocatorIndex = (m_currentAllocatorIndex + 1) % NUM_FRAME_BUFFERS;

    // FIX #9: Track startup phase to prevent premature unloading
    // Once we've generated the initial batch of chunks, exit startup phase
    if (m_startupPhase) {
        m_chunksGeneratedSinceStartup++;
        if (m_chunksGeneratedSinceStartup >= m_config.maxQueuedChunks) {
            m_startupPhase = false;
            spdlog::info("Startup phase complete - {} chunks generated, unloading now enabled",
                m_chunksGeneratedSinceStartup);
        }
    }

    // spdlog::debug("Chunk [{},{},{}] generation submitted (fence {}), {} chunks loaded, next allocator: {}",
    //     coord.x, coord.y, coord.z, m_chunkFenceValue, m_loadedChunks.size(), m_currentAllocatorIndex);

    return {};
}

void InfiniteChunkManager::UnloadDistantChunks(const ChunkCoord& cameraChunk) {
    // FIX #9: CRITICAL - Don't unload chunks during startup phase!
    // During startup, chunks are being queued/generated for the first time.
    // Unloading them before they're all generated causes:
    // 1. Fence waits on chunks that haven't even started GPU work (5 second timeout × N chunks)
    // 2. Wasted work - chunk queued, then unloaded before generation
    // 3. Queue thrashing - same chunks queue/unload repeatedly
    if (m_startupPhase) {
        spdlog::debug("Skipping chunk unload during startup phase ({}/{} chunks generated)",
            m_chunksGeneratedSinceStartup, m_config.maxQueuedChunks);
        return;  // Don't unload until initial batch is fully generated
    }

    // Iterate and unload chunks beyond unload distance
    for (auto it = m_loadedChunks.begin(); it != m_loadedChunks.end(); ) {
        const ChunkCoord& coord = it->first;

        // FIX: Only check horizontal distance, keep terrain layers always loaded
        // Unload chunks that are outside the terrain vertical range OR too far horizontally
        bool outsideTerrainLayers = (coord.y < TERRAIN_CHUNK_MIN_Y ||
                                      coord.y >= TERRAIN_CHUNK_MIN_Y + TERRAIN_NUM_CHUNKS_Y);

        // FIX: Calculate horizontal distance using SQUARE (Chebyshev) metric
        // Must match the square loading pattern, not circular
        int32_t dx = coord.x - cameraChunk.x;
        int32_t dz = coord.z - cameraChunk.z;
        int32_t horizontalDist = std::max(std::abs(dx), std::abs(dz));  // Chebyshev distance

        bool beyondHorizontal = horizontalDist > m_config.unloadDistanceHorizontal;

        // Unload if outside terrain layers OR beyond horizontal distance
        if (outsideTerrainLayers || beyondHorizontal) {
            // CRITICAL FIX #7: Don't unload chunks that are still generating!
            // This prevents CPU-GPU deadlock when trying to unload mid-generation chunks
            Chunk* chunk = it->second;
            if (chunk && chunk->GetState() == ChunkState::GenerationSubmitted) {
                spdlog::debug("Chunk [{},{},{}] is still generating (state: GenerationSubmitted), deferring unload",
                    coord.x, coord.y, coord.z);
                ++it;  // Skip this chunk, will retry next frame
                continue;
            }

            // CRITICAL FIX #8: Use timeout instead of INFINITE wait to prevent deadlock
            // If GPU hangs, INFINITE wait would freeze the entire system
            auto fenceIt = m_chunkGenerationFences.find(coord);
            if (fenceIt != m_chunkGenerationFences.end()) {
                uint64_t chunkFenceValue = fenceIt->second;
                uint64_t completedValue = m_chunkFence->GetCompletedValue();

                // Only wait if GPU hasn't finished yet
                if (completedValue < chunkFenceValue) {
                    spdlog::debug("Waiting for chunk [{},{},{}] GPU fence {} (completed: {})",
                        coord.x, coord.y, coord.z, chunkFenceValue, completedValue);

                    HRESULT hr = m_chunkFence->SetEventOnCompletion(chunkFenceValue, m_chunkFenceEvent);
                    if (SUCCEEDED(hr)) {
                        // CRITICAL: 5 second timeout instead of INFINITE to prevent system freeze
                        DWORD waitResult = WaitForSingleObject(m_chunkFenceEvent, 5000);
                        if (waitResult == WAIT_TIMEOUT) {
                            spdlog::error("Chunk [{},{},{}] fence wait timeout - GPU may be hung! Skipping unload.",
                                coord.x, coord.y, coord.z);
                            ++it;  // Skip this chunk to prevent deadlock
                            continue;
                        } else if (waitResult != WAIT_OBJECT_0) {
                            spdlog::error("Chunk [{},{},{}] fence wait failed: {:#x}",
                                coord.x, coord.y, coord.z, static_cast<uint32_t>(waitResult));
                            ++it;  // Skip on error
                            continue;
                        }
                    } else {
                        spdlog::error("Failed to set fence event for chunk unload: {:#x}", static_cast<uint32_t>(hr));
                        ++it;  // Skip on error
                        continue;
                    }
                }
            }

            // CACHE FIX: Notify callback BEFORE erasing so VoxelWorld can clean up its copy cache
            if (m_unloadCallback) {
                m_unloadCallback(coord);
            }

            // FIX #19: CRITICAL - Queue ENTIRE chunk for deferred delete to prevent GPU crash
            // The bug: Deleting chunks immediately while GPU might still be using buffers from
            // previous frames causes D3D12 ERROR: OBJECT_DELETED_WHILE_STILL_IN_USE crash
            // Solution: Queue entire chunk for deferred delete, will delete 10 frames later when GPU is done
            if (it->second) {
                Chunk* chunk = it->second;

                // Queue entire chunk for deferred deletion (includes buffers AND descriptors)
                DeferredChunkDelete deferredDelete;
                deferredDelete.chunk = chunk;
                // Delete 10 frames later - ensures GPU has finished using chunk buffers
                // (3 frame ring buffer × 3 = 9 frames safety margin, rounded to 10)
                deferredDelete.fenceValue = m_chunkFenceValue + 10;

                m_deferredChunkDeletes.push_back(deferredDelete);
                spdlog::debug("Chunk [{},{},{}] queued for deferred delete (fence {})",
                    coord.x, coord.y, coord.z, deferredDelete.fenceValue);
            }

            // FIX #2: Remove from pending generation fences to prevent stale entries
            m_chunkGenerationFences.erase(coord);

            // Clear re-queue counter for unloaded chunks
            m_chunkRequeueCount.erase(coord);

            spdlog::debug("Unloaded chunk [{},{},{}] - distance: horiz={} (outsideTerrainLayers={})",
                coord.x, coord.y, coord.z, horizontalDist, outsideTerrainLayers);

            it = m_loadedChunks.erase(it);
        } else {
            ++it;
        }
    }
}

Result<void> InfiniteChunkManager::CreateGenerationPipeline(ID3D12Device* device) {
    // ===== COMPILE SHADER =====
    Graphics::ShaderCompiler compiler;
    auto initResult = compiler.Initialize();
    if (!initResult) {
        return Error("Failed to initialize shader compiler: {}", initResult.error());
    }

    std::filesystem::path shaderPath = "assets/shaders/Compute/CS_GenerateChunk.hlsl";
    auto compileResult = compiler.CompileComputeShader(shaderPath, L"main", true);
    if (!compileResult) {
        return Error("Failed to compile CS_GenerateChunk.hlsl: {}", compileResult.error());
    }

    auto& compiledShader = compileResult.value();
    if (!compiledShader.IsValid()) {
        return Error("CS_GenerateChunk.hlsl compilation failed: {}", compiledShader.errors);
    }

    // ===== CREATE ROOT SIGNATURE =====
    // Root parameter 0: CBV (ChunkConstants at b0)
    // Root parameter 1: UAV (ChunkVoxelOutput at u0)

    D3D12_ROOT_PARAMETER1 rootParams[2] = {};

    // Parameter 0: CBV
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: UAV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 2;
    rootSigDesc.Desc_1_1.pParameters = rootParams;
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
        return Error("Failed to serialize root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_generationRootSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create root signature");
    }

    // ===== CREATE PIPELINE STATE =====
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_generationRootSignature.Get();
    psoDesc.CS = compiledShader.GetBytecode();
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_generationPSO));
    if (FAILED(hr)) {
        return Error("Failed to create compute pipeline state");
    }

    m_generationPSO->SetName(L"CS_GenerateChunk_PSO");
    m_generationRootSignature->SetName(L"CS_GenerateChunk_RootSig");

    spdlog::info("Generation pipeline created successfully");

    // ===== CREATE SHARED CONSTANT BUFFER (CRITICAL OPTIMIZATION!) =====
    // Instead of creating a new constant buffer for each chunk (extremely expensive!),
    // create ONE buffer and reuse it for all chunks by updating its contents
    // ChunkConstants is 32 bytes, align to D3D12 CB alignment (256 bytes)
    uint64_t alignedSize = 256;

    D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(alignedSize);

    hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_sharedConstantBuffer)
    );

    if (FAILED(hr)) {
        return Error("Failed to create shared constant buffer");
    }

    m_sharedConstantBuffer->SetName(L"ChunkGeneration_SharedCB");

    // Persistent mapping (keep it mapped for entire lifetime)
    D3D12_RANGE readRange = {0, 0};  // CPU won't read
    hr = m_sharedConstantBuffer->Map(0, &readRange, &m_sharedConstantBufferMappedPtr);
    if (FAILED(hr)) {
        return Error("Failed to map shared constant buffer");
    }

    spdlog::info("Shared constant buffer created (256 bytes, persistent mapping)");
    return {};
}

// FIX #19: Process deferred chunk deletions
// This deletes chunks (buffers + descriptors) after GPU is guaranteed to be done using them
void InfiniteChunkManager::ProcessDeferredChunkDeletes() {
    if (!m_chunkFence) {
        return;  // No fence available
    }

    // Get current GPU fence value (what GPU has completed)
    uint64_t completedFenceValue = m_chunkFence->GetCompletedValue();

    // Process all deferred deletes that are now safe
    for (auto it = m_deferredChunkDeletes.begin(); it != m_deferredChunkDeletes.end(); ) {
        if (completedFenceValue >= it->fenceValue) {
            // Safe to delete now - GPU has finished with this chunk's buffers and descriptors
            if (it->chunk) {
                it->chunk->Shutdown();  // Frees GPU buffers and descriptors
                delete it->chunk;
            }

            spdlog::debug("Deferred chunk delete complete (fence {} >= {})",
                completedFenceValue, it->fenceValue);

            // Remove from deferred list
            it = m_deferredChunkDeletes.erase(it);
        } else {
            // Not ready yet, keep waiting
            ++it;
        }
    }

    // Log if we have pending deferred deletes
    if (!m_deferredChunkDeletes.empty()) {
        spdlog::trace("{} deferred chunk deletes pending (GPU fence: {}, oldest waiting for: {})",
            m_deferredChunkDeletes.size(), completedFenceValue,
            m_deferredChunkDeletes.front().fenceValue);
    }
}

// CRITICAL FIX: Public wrapper for VerifyGeneratedChunks
// Call this before UpdateActiveRegion to catch chunks that just finished generating
void InfiniteChunkManager::PollCompletedChunks() {
    VerifyGeneratedChunks();
}

// FIX #1/#3: Poll GPU fence and mark chunks as Generated when GPU completes
void InfiniteChunkManager::VerifyGeneratedChunks() {
    if (!m_chunkFence) {
        return;  // No fence created yet
    }

    // Get current GPU fence value (what has completed)
    uint64_t completedFenceValue = m_chunkFence->GetCompletedValue();

    // DIAGNOSTIC: Track verification results
    int chunksCompleted = 0;
    int chunksStillPending = 0;

    // Iterate through all chunks waiting for generation to complete
    for (auto it = m_chunkGenerationFences.begin(); it != m_chunkGenerationFences.end(); ) {
        const ChunkCoord& coord = it->first;
        uint64_t chunkFenceValue = it->second;

        // Check if GPU has completed this chunk's generation
        if (completedFenceValue >= chunkFenceValue) {
            // GPU finished generating this chunk!
            auto chunkIt = m_loadedChunks.find(coord);
            if (chunkIt != m_loadedChunks.end() && chunkIt->second) {
                Chunk* chunk = chunkIt->second;

                // Mark chunk as Generated (now safe to copy/render)
                chunk->MarkGenerated();
                chunksCompleted++;

                // spdlog::debug("Chunk [{},{},{}] generation COMPLETED (fence {} signaled)",
                //     coord.x, coord.y, coord.z, chunkFenceValue);
            }

            // Remove from pending list
            it = m_chunkGenerationFences.erase(it);
        } else {
            // Still waiting for GPU
            chunksStillPending++;
            ++it;
        }
    }

    // DIAGNOSTIC: Log if we have chunks stuck in pending state
    if (chunksStillPending > 100) {
        static int logThrottle = 0;
        if (++logThrottle % 60 == 1) {  // Log once per second
            spdlog::warn("VerifyGeneratedChunks: {} chunks still pending GPU (completed={}, oldest fence={})",
                chunksStillPending, completedFenceValue,
                m_chunkGenerationFences.empty() ? 0 : m_chunkGenerationFences.begin()->second);
        }
    }
}

} // namespace VENPOD::Simulation
