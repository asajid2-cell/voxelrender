#include "InfiniteChunkManager.h"
#include "TerrainConstants.h"
#include <d3d12.h>
#include "../Graphics/RHI/d3dx12.h"
#include "../Graphics/RHI/ShaderCompiler.h"
#include "../Graphics/RHI/DX12ComputePipeline.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <utility>

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
    //   - Fence check: if (UINT64_MAX != UINT64_MAX) -> FALSE, but then...
    //   - Wait check: 0 < UINT64_MAX -> TRUE, thinks allocator is BUSY!
    //   - Result: All 3 allocators fail busy check on Frame 0 -> no chunks generate
    // Correct logic: Use 0 = "never used", check `if (value > 0)` to detect actual usage
    for (uint32_t i = 0; i < NUM_FRAME_BUFFERS; ++i) {
        m_allocatorFenceValues[i] = 0;  // 0 = ready for immediate use (never used)
    }

    spdlog::info("InfiniteChunkManager initialized - load XZ +/-{}, load Y -{}/+{}, unload XZ +/-{}, unload Y -{}/+{}, seed: {}",
        m_config.loadDistanceHorizontal,
        m_config.loadDistanceVerticalBelow,
        m_config.loadDistanceVerticalAbove,
        m_config.unloadDistanceHorizontal,
        m_config.unloadDistanceVerticalBelow,
        m_config.unloadDistanceVerticalAbove,
        m_config.worldSeed);
    spdlog::info("Terrain generation: vertical world Y={}..{} (chunks {}..{}), rendered through a moving {}-chunk Y window",
        TERRAIN_MIN_Y, TERRAIN_MAX_Y,
        TERRAIN_CHUNK_MIN_Y, TERRAIN_CHUNK_MAX_Y,
        RENDER_BUFFER_CHUNKS_Y);
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

    // Clear generation queue (priority_queue has no clear() method)
    while (!m_generationQueue.empty()) {
        m_generationQueue.pop();
    }
    m_queuedChunks.clear();

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

    // ===== STEP 1: Calculate camera's chunk coordinate in full 3D =====
    // Keep the raw camera chunk for diagnostics, but clamp the streaming Y
    // center so flying above/below the conceptual terrain range does not unload
    // every real terrain chunk and leave the player looking at an empty window.
    ChunkCoord rawCameraChunk = ChunkCoord::FromWorldPosition(
        static_cast<int32_t>(std::floor(cameraWorldPos.x)),
        static_cast<int32_t>(std::floor(cameraWorldPos.y)),
        static_cast<int32_t>(std::floor(cameraWorldPos.z)),
        INFINITE_CHUNK_SIZE
    );
    ChunkCoord cameraChunk = rawCameraChunk;
    cameraChunk.y = ClampVerticalChunkCenter(
        rawCameraChunk.y,
        m_config.loadDistanceVerticalBelow,
        m_config.loadDistanceVerticalAbove);

    // PRIORITY 2: Store camera chunk for external access (chunk scan optimization)
    m_cameraChunk = rawCameraChunk;

    bool shouldQueueChunks = true;
    const bool cameraMoved = cameraChunk != m_lastCameraChunk;

    if (cameraMoved) {
        // Keep existing pending work instead of destroying it on every chunk
        // boundary. Most queued chunks are still inside the new load window, and
        // repeatedly clearing the queue prevents render-window edge chunks from
        // ever finishing during fast movement. GenerateNextChunk drops truly
        // stale entries when they reach the front of the queue.
        m_stationaryFrameCount = 0;
    }

    // Only queue new chunks if camera moved or the current queue drained.
    if (!cameraMoved) {
        // CRITICAL FIX: Continue generating queued chunks even when stationary
        // The previous logic had a deadlock: after 3 chunks, it would return if queue wasn't empty,
        // causing 20+ chunks to remain stuck in queue forever -> system freeze
        m_stationaryFrameCount++;

        // If we have queued chunks, skip re-queueing and fall through to the
        // batched generation loop below. Returning here limited startup to one
        // chunk per frame.
        if (!m_generationQueue.empty()) {
            shouldQueueChunks = false;
        }
        else {
            // Queue only a bounded nearest-missing batch when caught up. The
            // old startup path tried to populate the whole load cube, which
            // created thousands of 1 MB source chunks and could push WDDM into
            // paging before the first playable frame.
            shouldQueueChunks = true;
            m_stationaryFrameCount = 0;
        }
    }

    m_lastCameraChunk = cameraChunk;

    // Log only when camera chunk changes (not every frame)
    static ChunkCoord lastLoggedChunk = {INT32_MAX, INT32_MAX, INT32_MAX};
    if (rawCameraChunk != lastLoggedChunk) {
        spdlog::debug("Camera chunk: raw=[{},{},{}] stream=[{},{},{}] - world pos: ({:.1f},{:.1f},{:.1f})",
            rawCameraChunk.x, rawCameraChunk.y, rawCameraChunk.z,
            cameraChunk.x, cameraChunk.y, cameraChunk.z,
            cameraWorldPos.x, cameraWorldPos.y, cameraWorldPos.z);
        lastLoggedChunk = rawCameraChunk;
    }

    // Free old resident chunks before admitting new work. This keeps the
    // source-chunk pool bounded; otherwise a stationary camera eventually
    // fills the entire load cube and can push 8 GB GPUs into VRAM paging.
    UnloadDistantChunks(cameraChunk);

    // ===== STEP 2: Queue chunks within cylindrical render distance =====
    if (shouldQueueChunks) {
        auto queueResult = QueueChunksAroundCamera(cameraChunk);
        if (!queueResult) {
            spdlog::warn("Failed to queue chunks: {}", queueResult.error());
        }
    }

    GenerateQueuedChunks(device, cmdQueue);
}

void InfiniteChunkManager::GenerateQueuedChunks(ID3D12Device* device, ID3D12CommandQueue* cmdQueue) {
    if (!device || !cmdQueue || m_generationQueue.empty() || m_config.chunksPerFrame == 0) {
        return;
    }

    if (m_loadedChunks.size() >= m_config.maxQueuedChunks) {
        TrimSourceCacheForVisibleWindow(m_lastCameraChunk);
        if (m_loadedChunks.size() >= m_config.maxQueuedChunks) {
            static uint32_t residentGenerationThrottle = 0;
            if (++residentGenerationThrottle % 120 == 1) {
                spdlog::debug("Chunk generation paused: resident source cache full ({}/{})",
                    m_loadedChunks.size(),
                    m_config.maxQueuedChunks);
            }
            return;
        }
    }

    // The generation path records a batch of chunk dispatches into one command
    // list/allocator. The allocator ring gates whether we can submit a batch at
    // all; it must not cap the number of chunks inside that batch. Capping the
    // per-frame chunk count to free allocator count throttled streaming to only
    // a few chunks per frame and let fast walking/flying outrun the world.
    const uint64_t completedFence = m_chunkFence->GetCompletedValue();
    size_t availableAllocatorCount = 0;
    for (uint32_t i = 0; i < NUM_FRAME_BUFFERS; ++i) {
        const uint64_t allocatorFenceValue = m_allocatorFenceValues[i];
        if (allocatorFenceValue == 0 || completedFence >= allocatorFenceValue) {
            ++availableAllocatorCount;
        }
    }

    if (availableAllocatorCount == 0) {
        static uint32_t generationWaitLogThrottle = 0;
        if (++generationWaitLogThrottle % 120 == 1) {
            spdlog::debug("Chunk generation ring busy; deferring queued work (completed fence {})",
                completedFence);
        }
    }

    size_t chunksPerFrame = (availableAllocatorCount == 0 || m_config.chunksPerFrame == 0)
        ? 0
        : std::clamp<size_t>(
            static_cast<size_t>(m_config.chunksPerFrame),
            1,
            64);

    if (chunksPerFrame > 0 && !m_generationQueue.empty()) {
        uint32_t allocatorIndex = m_currentAllocatorIndex;
        uint32_t triesRemaining = NUM_FRAME_BUFFERS;
        while (triesRemaining > 0) {
            const uint64_t allocatorFenceValue = m_allocatorFenceValues[allocatorIndex];
            if (allocatorFenceValue == 0 || completedFence >= allocatorFenceValue) {
                break;
            }
            allocatorIndex = (allocatorIndex + 1) % NUM_FRAME_BUFFERS;
            --triesRemaining;
        }

        if (triesRemaining == 0) {
            static uint32_t busyLogThrottle = 0;
            if (++busyLogThrottle % 60 == 1) {
                spdlog::debug("All chunk generation allocators busy (completed fence {}), deferring generation",
                    completedFence);
            }
        } else if (!m_heapManager || !m_chunkCmdAllocators[allocatorIndex] || !m_chunkCmdList) {
            spdlog::critical("FATAL: Chunk generation batch resources are not initialized");
        } else {
            HRESULT hr = m_chunkCmdAllocators[allocatorIndex]->Reset();
            if (FAILED(hr)) {
                spdlog::error("Failed to reset batch chunk cmd allocator {} (HRESULT={:#x})",
                    allocatorIndex, static_cast<uint32_t>(hr));
            } else {
                hr = m_chunkCmdList->Reset(m_chunkCmdAllocators[allocatorIndex].Get(), nullptr);
                if (FAILED(hr)) {
                    spdlog::error("Failed to reset batch chunk cmd list (HRESULT={:#x})",
                        static_cast<uint32_t>(hr));
                } else {
                    auto* heap = m_heapManager->GetShaderVisibleCbvSrvUavHeap();
                    const uint32_t maxDescriptors = heap ? heap->GetDesc().NumDescriptors : 0;
                    constexpr uint32_t DESCRIPTORS_PER_CHUNK = 2;
                    constexpr uint32_t SAFETY_MARGIN = 10;

                    std::vector<std::pair<ChunkCoord, Chunk*>> submittedChunks;
                    submittedChunks.reserve(chunksPerFrame);

                    while (submittedChunks.size() < chunksPerFrame &&
                           !m_generationQueue.empty() &&
                           m_loadedChunks.size() < m_config.maxQueuedChunks) {
                        ChunkPriorityEntry entry = m_generationQueue.top();
                        m_generationQueue.pop();
                        m_queuedChunks.erase(entry.coord);

                        const ChunkCoord coord = entry.coord;
                        if (!IsWithinLoadWindow(coord, m_lastCameraChunk)) {
                            m_chunkRequeueCount.erase(coord);
                            continue;
                        }
                        if (m_loadedChunks.find(coord) != m_loadedChunks.end() ||
                            m_chunkGenerationFences.find(coord) != m_chunkGenerationFences.end()) {
                            continue;
                        }

                        const uint32_t currentDescriptors =
                            m_heapManager->GetShaderVisibleCbvSrvUavAllocatedCount();
                        if (!heap ||
                            currentDescriptors + DESCRIPTORS_PER_CHUNK + SAFETY_MARGIN > maxDescriptors) {
                            const uint32_t requeueCount = ++m_chunkRequeueCount[coord];
                            constexpr uint32_t MAX_REQUEUE_ATTEMPTS = 50;
                            if (requeueCount <= MAX_REQUEUE_ATTEMPTS) {
                                EnqueueChunk(entry);
                            } else {
                                spdlog::warn("Chunk [{},{},{}] dropped due to descriptor exhaustion after {} attempts (heap: {}/{})",
                                    coord.x, coord.y, coord.z, MAX_REQUEUE_ATTEMPTS, currentDescriptors, maxDescriptors);
                                m_chunkRequeueCount.erase(coord);
                            }
                            break;
                        }

                        Chunk* chunk = new Chunk();
                        auto initResult = chunk->Initialize(device, *m_heapManager, coord, "InfiniteChunk");
                        if (!initResult) {
                            delete chunk;
                            spdlog::error("Failed to initialize chunk [{},{},{}]: {}",
                                coord.x, coord.y, coord.z, initResult.error());
                            continue;
                        }

                        auto genResult = chunk->Generate(
                            device,
                            m_chunkCmdList.Get(),
                            m_generationPSO.Get(),
                            m_generationRootSignature.Get(),
                            m_sharedConstantBuffer.Get(),
                            m_sharedConstantBufferMappedPtr,
                            m_config.worldSeed);
                        if (!genResult) {
                            chunk->Shutdown();
                            delete chunk;
                            spdlog::error("Failed to generate chunk [{},{},{}]: {}",
                                coord.x, coord.y, coord.z, genResult.error());
                            continue;
                        }

                        m_loadedChunks[coord] = chunk;
                        m_chunkRequeueCount.erase(coord);
                        submittedChunks.emplace_back(coord, chunk);
                    }

                    if (!submittedChunks.empty()) {
                        m_chunkCmdList->Close();
                        ID3D12CommandList* lists[] = { m_chunkCmdList.Get() };
                        cmdQueue->ExecuteCommandLists(1, lists);

                        ++m_chunkFenceValue;
                        cmdQueue->Signal(m_chunkFence.Get(), m_chunkFenceValue);
                        m_allocatorFenceValues[allocatorIndex] = m_chunkFenceValue;

                        for (const auto& [coord, chunk] : submittedChunks) {
                            (void)chunk;
                            m_chunkGenerationFences[coord] = m_chunkFenceValue;
                        }

                        m_currentAllocatorIndex = (allocatorIndex + 1) % NUM_FRAME_BUFFERS;

                        if (m_startupPhase) {
                            m_chunksGeneratedSinceStartup += static_cast<uint32_t>(submittedChunks.size());
                            const uint32_t startupReadyTarget = std::min<uint32_t>(
                                m_config.maxQueuedChunks,
                                static_cast<uint32_t>(RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y * RENDER_BUFFER_CHUNKS_Z));
                            if (m_chunksGeneratedSinceStartup >= startupReadyTarget) {
                                m_startupPhase = false;
                                spdlog::info("Startup phase complete - {} chunks generated (target {}), unloading now enabled",
                                    m_chunksGeneratedSinceStartup,
                                    startupReadyTarget);
                            }
                        }
                    } else {
                        m_chunkCmdList->Close();
                        m_allocatorFenceValues[allocatorIndex] = 0;
                    }
                }
            }
        }
    }

    // Log only once per second to avoid spam
    static int chunkStatsThrottle = 0;
    if (++chunkStatsThrottle % 60 == 1) {
        spdlog::debug("Chunks loaded: {}, queued: {}",
            m_loadedChunks.size(),
            m_generationQueue.size());
    }
}

void InfiniteChunkManager::EnsureQueuedAroundChunk(const ChunkCoord& centerChunk) {
    // The dense render cache can intentionally lag the camera by a few chunks
    // because of recenter hysteresis. Source streaming must follow that active
    // render center, otherwise the copy pass asks for chunks that were never
    // queued and the world looks like a partial island.
    m_cameraChunk = centerChunk;
    m_lastCameraChunk = centerChunk;
    QueueChunksAroundCamera(centerChunk);
}

bool InfiniteChunkManager::QueueUrgentChunk(const ChunkCoord& coord, int32_t priority) {
    if (coord.y < TERRAIN_CHUNK_MIN_Y || coord.y > TERRAIN_CHUNK_MAX_Y) {
        return false;
    }
    if (m_loadedChunks.find(coord) != m_loadedChunks.end() ||
        m_chunkGenerationFences.find(coord) != m_chunkGenerationFences.end() ||
        IsChunkQueued(coord)) {
        return false;
    }

    if (m_loadedChunks.size() + m_generationQueue.size() >= m_config.maxQueuedChunks) {
        TrimSourceCacheForVisibleWindow(m_lastCameraChunk);
        if (m_loadedChunks.size() + m_generationQueue.size() >= m_config.maxQueuedChunks) {
            // Visible demand is more important than speculative queued work.
            PruneGenerationQueueForCenter(m_lastCameraChunk);
        }
    }

    return EnqueueChunk(ChunkPriorityEntry{coord, priority});
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
    PruneGenerationQueueForCenter(cameraChunk);

    // Limit resident source chunks plus queued work. Each generated chunk owns
    // about 1 MB of GPU source data in addition to the dense render buffers.
    // Letting a stationary camera fill the whole load cube can overcommit VRAM
    // and produce multi-second WDDM paging stalls.
    struct Candidate {
        ChunkCoord coord;
        int32_t priority;
        bool visible;
    };
    std::vector<Candidate> candidates;
    size_t visibleMissingCandidates = 0;
    int32_t yMin = std::max(TERRAIN_CHUNK_MIN_Y, cameraChunk.y - m_config.loadDistanceVerticalBelow);
    int32_t yMax = std::min(TERRAIN_CHUNK_MAX_Y, cameraChunk.y + m_config.loadDistanceVerticalAbove);
    int32_t yCount = std::max<int32_t>(0, yMax - yMin + 1);
    candidates.reserve(static_cast<size_t>(
        (m_config.loadDistanceHorizontal * 2 + 1) *
        (m_config.loadDistanceHorizontal * 2 + 1) *
        yCount));

    // ===== 3D VERTICAL WINDOW + HORIZONTAL LOADING =====
    // Queue chunk layers around the player's current Y chunk, clamped to the
    // conceptual terrain range. This keeps the loaded set bounded while allowing
    // the visible window to travel from deep ravines to high spires.
    for (int32_t chunkY = yMin; chunkY <= yMax; ++chunkY) {
        for (int32_t dx = -m_config.loadDistanceHorizontal; dx <= m_config.loadDistanceHorizontal; ++dx) {
            for (int32_t dz = -m_config.loadDistanceHorizontal; dz <= m_config.loadDistanceHorizontal; ++dz) {
                // FIX: Use SQUARE pattern to match VoxelWorld renderer expectations
                // The renderer scans a square grid (25x25), so we must load chunks in a square pattern
                // Previously used circular radius which skipped corners -> 124 missing chunks -> holes
                // Now using Chebyshev distance (max of abs(dx), abs(dz)) for square coverage
                //
                // Example with renderDistance=12:
                // - Square: loads all chunks where |dx| <= 12 AND |dz| <= 12 -> 25x25 = 625 chunks
                // - Circle: loads chunks where dx+dz <= 144 -> ~501 chunks (misses 124 corners!)
                //
                // No distance check needed - the loop bounds already define the square:
                // for dx in [-renderDistance, +renderDistance]
                // for dz in [-renderDistance, +renderDistance]
                // All chunks within this square are needed by the renderer

                ChunkCoord coord = {
                    cameraChunk.x + dx,
                    chunkY,
                    cameraChunk.z + dz
                };

                // Check if already loaded OR already queued for generation
                if (m_loadedChunks.find(coord) != m_loadedChunks.end()) {
                    continue;  // Already loaded
                }

                // CRITICAL FIX: Check if already pending generation (prevents duplicate queuing)
                if (m_chunkGenerationFences.find(coord) != m_chunkGenerationFences.end() ||
                    IsChunkQueued(coord)) {
                    continue;  // Already queued/generating
                }

                const int32_t dy = chunkY - cameraChunk.y;
                const bool inDenseVisibleWindow =
                    std::abs(dx) <= RENDER_DISTANCE_HORIZONTAL &&
                    std::abs(dz) <= RENDER_DISTANCE_HORIZONTAL &&
                    chunkY >= cameraChunk.y - RENDER_DISTANCE_VERTICAL_BELOW &&
                    chunkY <= cameraChunk.y + RENDER_DISTANCE_VERTICAL_ABOVE;
                const int32_t chebyshev = std::max(std::abs(dx), std::abs(dz));
                const int32_t priority =
                    (inDenseVisibleWindow ? 0 : 1'000'000) +
                    chebyshev * 1024 +
                    std::abs(dy) * 16 +
                    (dx * dx + dz * dz);
                candidates.push_back(Candidate{coord, priority, inDenseVisibleWindow});
                if (inDenseVisibleWindow) {
                    ++visibleMissingCandidates;
                }
            }
        }
    }

    const size_t denseVisibleChunkCount = static_cast<size_t>(
        (RENDER_DISTANCE_HORIZONTAL * 2 + 1) *
        RENDER_BUFFER_CHUNKS_Y *
        (RENDER_DISTANCE_HORIZONTAL * 2 + 1));
    const size_t maxPendingGenerationQueue = std::max<size_t>(
        std::max<size_t>(
            static_cast<size_t>(m_config.maxChunksQueuedPerUpdate) * 8u,
            denseVisibleChunkCount),
        384u);

    size_t residentCount = m_loadedChunks.size();
    size_t pendingCount = m_generationQueue.size();
    if (residentCount >= m_config.maxQueuedChunks && visibleMissingCandidates > 0) {
        TrimSourceCacheForVisibleWindow(cameraChunk);
        residentCount = m_loadedChunks.size();
        pendingCount = m_generationQueue.size();
    }
    if (residentCount >= m_config.maxQueuedChunks) {
        static uint32_t budgetLogThrottle = 0;
        if (++budgetLogThrottle % 120 == 1) {
            spdlog::debug("Chunk resident budget full (resident={}, queued={}, budget={}, missingVisible={}), skipping new queueing",
                residentCount, pendingCount, m_config.maxQueuedChunks, visibleMissingCandidates);
        }
        return {};  // Queue is full, wait for existing chunks to generate or unload
    }

    if (pendingCount >= maxPendingGenerationQueue) {
        if (visibleMissingCandidates > 0) {
            PruneGenerationQueueForCenter(cameraChunk);
            pendingCount = m_generationQueue.size();
        }
        if (pendingCount >= maxPendingGenerationQueue) {
            static uint32_t pendingLogThrottle = 0;
            if (++pendingLogThrottle % 120 == 1) {
                spdlog::debug("Chunk generation backlog full (resident={}, queued={}, pendingCap={}, missingVisible={}), waiting for generation",
                    residentCount, pendingCount, maxPendingGenerationQueue, visibleMissingCandidates);
            }
            return {};
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        if (a.coord.y != b.coord.y) {
            return a.coord.y < b.coord.y;
        }
        if (a.coord.x != b.coord.x) {
            return a.coord.x < b.coord.x;
        }
        return a.coord.z < b.coord.z;
    });

    size_t remainingResidentCapacity = m_config.maxQueuedChunks - residentCount;
    size_t remainingPendingCapacity = maxPendingGenerationQueue - pendingCount;
    size_t queuedCount = std::min({
        remainingResidentCapacity,
        remainingPendingCapacity,
        candidates.size(),
        static_cast<size_t>(std::max<uint32_t>(1, m_config.maxChunksQueuedPerUpdate))
    });

    // Add new chunks to generation queue with true distance priority. The old
    // code stopped while scanning dx=-loadDistance, so startup queued a far
    // edge of terrain before the chunks under the player.
    size_t actualQueuedCount = 0;
    for (size_t i = 0; i < queuedCount; ++i) {
        if (EnqueueChunk(ChunkPriorityEntry{candidates[i].coord, candidates[i].priority})) {
            ++actualQueuedCount;
        }
    }

    if (actualQueuedCount > 0) {
        static uint32_t queueLogThrottle = 0;
        if (actualQueuedCount >= 64 || (++queueLogThrottle % 30u) == 1u) {
            spdlog::debug("Queued {} new chunks for generation at Y={}..{} (queue size: {}/{}, resident={}) - load distance: {} chunks",
                actualQueuedCount, yMin, yMax,
                m_generationQueue.size(), m_config.maxQueuedChunks,
                m_loadedChunks.size(),
                m_config.loadDistanceHorizontal);
        }

        // Log first few chunks being queued for verification
        static bool firstLog = true;
        if (firstLog) {
            spdlog::info("Seamless streaming: loading chunks up to {} chunks away (visible at {} chunks)",
                m_config.loadDistanceHorizontal, RENDER_DISTANCE_HORIZONTAL);
            spdlog::info("First chunks queued by distance (all at Y=0 or Y=1):");
            for (size_t i = 0; i < std::min<size_t>(5, queuedCount); ++i) {
                const auto& coord = candidates[i].coord;
            spdlog::info("  Chunk [{},{},{}] priority={}",
                    coord.x, coord.y, coord.z, candidates[i].priority);
            }
            firstLog = false;
        }
    }
    return {};
}

bool InfiniteChunkManager::IsChunkQueued(const ChunkCoord& coord) const {
    return m_queuedChunks.find(coord) != m_queuedChunks.end();
}

bool InfiniteChunkManager::EnqueueChunk(const ChunkPriorityEntry& entry) {
    if (m_loadedChunks.find(entry.coord) != m_loadedChunks.end() ||
        m_chunkGenerationFences.find(entry.coord) != m_chunkGenerationFences.end() ||
        IsChunkQueued(entry.coord)) {
        return false;
    }

    m_generationQueue.push(entry);
    m_queuedChunks.insert(entry.coord);
    return true;
}

void InfiniteChunkManager::PruneGenerationQueueForCenter(const ChunkCoord& centerChunk) {
    if (m_generationQueue.empty()) {
        m_queuedChunks.clear();
        return;
    }

    std::vector<ChunkPriorityEntry> retained;
    retained.reserve(m_generationQueue.size());
    size_t dropped = 0;
    m_queuedChunks.clear();

    while (!m_generationQueue.empty()) {
        ChunkPriorityEntry entry = m_generationQueue.top();
        m_generationQueue.pop();

        if (m_loadedChunks.find(entry.coord) != m_loadedChunks.end() ||
            m_chunkGenerationFences.find(entry.coord) != m_chunkGenerationFences.end()) {
            ++dropped;
            continue;
        }
        if (!IsWithinLoadWindow(entry.coord, centerChunk)) {
            m_chunkRequeueCount.erase(entry.coord);
            ++dropped;
            continue;
        }

        const int32_t dx = entry.coord.x - centerChunk.x;
        const int32_t dy = entry.coord.y - centerChunk.y;
        const int32_t dz = entry.coord.z - centerChunk.z;
        const bool inDenseVisibleWindow =
            std::abs(dx) <= RENDER_DISTANCE_HORIZONTAL &&
            std::abs(dz) <= RENDER_DISTANCE_HORIZONTAL &&
            entry.coord.y >= centerChunk.y - RENDER_DISTANCE_VERTICAL_BELOW &&
            entry.coord.y <= centerChunk.y + RENDER_DISTANCE_VERTICAL_ABOVE;
        const int32_t chebyshev = std::max(std::abs(dx), std::abs(dz));
        entry.priority =
            (inDenseVisibleWindow ? 0 : 1'000'000) +
            chebyshev * 1024 +
            std::abs(dy) * 16 +
            (dx * dx + dz * dz);
        retained.push_back(entry);
    }

    for (const auto& entry : retained) {
        EnqueueChunk(entry);
    }

    if (dropped > 0) {
        static uint32_t pruneLogThrottle = 0;
        if (++pruneLogThrottle % 120 == 1) {
            spdlog::debug("Pruned {} stale queued chunks for active center [{},{},{}] (retained {})",
                dropped, centerChunk.x, centerChunk.y, centerChunk.z, retained.size());
        }
    }
}

Result<void> InfiniteChunkManager::GenerateNextChunk(
    ID3D12Device* device,
    ID3D12CommandQueue* cmdQueue)  // CHANGED: Use queue for immediate execution
{
    if (m_generationQueue.empty()) {
        return {};
    }

    // Pop highest priority chunk (nearest to the center when it was queued).
    // Discard stale entries that are no longer inside the current load window;
    // this avoids old far work blocking new near chunks without throwing away
    // still-useful pending chunks on every camera chunk transition.
    ChunkPriorityEntry entry;
    ChunkCoord coord;
    bool foundRelevantEntry = false;
    while (!m_generationQueue.empty()) {
        entry = m_generationQueue.top();
        m_generationQueue.pop();
        coord = entry.coord;
        m_queuedChunks.erase(coord);

        if (!IsWithinLoadWindow(coord, m_lastCameraChunk)) {
            m_chunkRequeueCount.erase(coord);
            continue;
        }

        foundRelevantEntry = true;
        break;
    }

    if (!foundRelevantEntry) {
        return {};
    }

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

    // ===== RING BUFFER FIX: Find an available allocator instead of retrying one busy slot =====
    uint32_t allocatorIndex = m_currentAllocatorIndex;
    uint64_t completedFence = m_chunkFence->GetCompletedValue();
    uint32_t triesRemaining = NUM_FRAME_BUFFERS;
    while (triesRemaining > 0) {
        uint64_t allocatorFenceValue = m_allocatorFenceValues[allocatorIndex];
        if (allocatorFenceValue == 0 || completedFence >= allocatorFenceValue) {
            break;
        }
        allocatorIndex = (allocatorIndex + 1) % NUM_FRAME_BUFFERS;
        triesRemaining--;
    }

    if (triesRemaining == 0) {
        EnqueueChunk(entry);
        static uint32_t busyLogThrottle = 0;
        if (++busyLogThrottle % 60 == 1) {
            spdlog::debug("All chunk generation allocators busy (completed fence {}), deferring generation",
                completedFence);
        }
        return {};
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

        // Heap is full - re-queue chunk without touching allocator (keep same priority)
        spdlog::debug("Descriptor heap full ({}/{} descriptors), deferring chunk [{},{},{}] (attempt {})",
            currentDescriptors, maxDescriptors, coord.x, coord.y, coord.z, requeueCount);
        EnqueueChunk(entry);  // Re-queue for later
        return {};  // Not an error, just deferring
    }

    // CRITICAL FIX: DO NOT advance allocator index yet!
    // We must only advance AFTER successful chunk creation, otherwise we consume
    // an allocator slot without creating a chunk -> allocator starvation

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
        EnqueueChunk(entry);  // Re-queue for retry (keep same priority)
        spdlog::error("Failed to reset chunk cmd allocator {} (HRESULT={:#x}), re-queuing chunk [{},{},{}]",
            allocatorIndex, static_cast<uint32_t>(hr), coord.x, coord.y, coord.z);
        return Error("Command allocator Reset() failed");
    }
    // spdlog::debug("SYNC CHECK: Allocator {} reset successful", allocatorIndex);

    hr = m_chunkCmdList->Reset(m_chunkCmdAllocators[allocatorIndex].Get(), nullptr);
    if (FAILED(hr)) {
        EnqueueChunk(entry);  // Re-queue for retry (keep same priority)
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
        const uint32_t startupReadyTarget = std::min<uint32_t>(
            m_config.maxQueuedChunks,
            static_cast<uint32_t>(RENDER_BUFFER_CHUNKS_X * RENDER_BUFFER_CHUNKS_Y * RENDER_BUFFER_CHUNKS_Z));
        if (m_chunksGeneratedSinceStartup >= startupReadyTarget) {
            m_startupPhase = false;
            spdlog::info("Startup phase complete - {} chunks generated (target {}), unloading now enabled",
                m_chunksGeneratedSinceStartup,
                startupReadyTarget);
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
    // 1. Fence waits on chunks that haven't even started GPU work (5 second timeout x N chunks)
    // 2. Wasted work - chunk queued, then unloaded before generation
    // 3. Queue thrashing - same chunks queue/unload repeatedly
    if (m_startupPhase) {
        static uint32_t startupUnloadLogThrottle = 0;
        if (++startupUnloadLogThrottle % 240 == 1) {
            spdlog::debug("Skipping chunk unload during startup phase ({}/{} chunks generated)",
                m_chunksGeneratedSinceStartup, m_config.maxQueuedChunks);
        }
        return;  // Don't unload until initial batch is fully generated
    }

    // Iterate and unload chunks beyond unload distance
    for (auto it = m_loadedChunks.begin(); it != m_loadedChunks.end(); ) {
        const ChunkCoord& coord = it->first;

        bool outsideTerrainRange = (coord.y < TERRAIN_CHUNK_MIN_Y || coord.y > TERRAIN_CHUNK_MAX_Y);

        // FIX: Calculate horizontal distance using SQUARE (Chebyshev) metric
        // Must match the square loading pattern, not circular
        int32_t dx = coord.x - cameraChunk.x;
        int32_t dz = coord.z - cameraChunk.z;
        int32_t horizontalDist = std::max(std::abs(dx), std::abs(dz));  // Chebyshev distance

        bool beyondHorizontal = horizontalDist > m_config.unloadDistanceHorizontal;
        bool beyondVertical =
            coord.y < cameraChunk.y - m_config.unloadDistanceVerticalBelow ||
            coord.y > cameraChunk.y + m_config.unloadDistanceVerticalAbove;

        if (outsideTerrainRange || beyondHorizontal || beyondVertical) {
            // CRITICAL FIX #7: Don't unload chunks that are still generating!
            // This prevents CPU-GPU deadlock when trying to unload mid-generation chunks
            Chunk* chunk = it->second;
            if (chunk && chunk->GetState() == ChunkState::GenerationSubmitted) {
                spdlog::debug("Chunk [{},{},{}] is still generating (state: GenerationSubmitted), deferring unload",
                    coord.x, coord.y, coord.z);
                ++it;  // Skip this chunk, will retry next frame
                continue;
            }

            // Runtime stability: never wait for a chunk generation fence from the
            // streaming/unload path. Waiting here turns an unload decision into a
            // whole-frame hitch; the next frame can retry once the fence is done.
            auto fenceIt = m_chunkGenerationFences.find(coord);
            if (fenceIt != m_chunkGenerationFences.end()) {
                uint64_t chunkFenceValue = fenceIt->second;
                uint64_t completedValue = m_chunkFence->GetCompletedValue();

                if (completedValue < chunkFenceValue) {
                    spdlog::trace("Chunk [{},{},{}] fence {} still pending (completed {}), deferring unload",
                        coord.x, coord.y, coord.z, chunkFenceValue, completedValue);
                    ++it;
                    continue;
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
                Chunk* chunkToDelete = it->second;

                // Queue entire chunk for deferred deletion (includes buffers AND descriptors)
                DeferredChunkDelete deferredDelete;
                deferredDelete.chunk = chunkToDelete;
                // Delete 10 frames later - ensures GPU has finished using chunk buffers
                // (3 frame ring buffer x 3 = 9 frames safety margin, rounded to 10)
                deferredDelete.fenceValue = m_chunkFenceValue + 10;

                m_deferredChunkDeletes.push_back(deferredDelete);
                spdlog::trace("Chunk [{},{},{}] queued for deferred delete (fence {})",
                    coord.x, coord.y, coord.z, deferredDelete.fenceValue);
            }

            // FIX #2: Remove from pending generation fences to prevent stale entries
            m_chunkGenerationFences.erase(coord);

            // Clear re-queue counter for unloaded chunks
            m_chunkRequeueCount.erase(coord);

            spdlog::trace("Unloaded chunk [{},{},{}] - distance: horiz={} outsideRange={} beyondVertical={}",
                coord.x, coord.y, coord.z, horizontalDist, outsideTerrainRange, beyondVertical);

            it = m_loadedChunks.erase(it);
        } else {
            ++it;
        }
    }
}

void InfiniteChunkManager::TrimSourceCacheForVisibleWindow(const ChunkCoord& cameraChunk) {
    if (m_loadedChunks.empty()) {
        return;
    }

    const size_t targetResident =
        m_config.maxQueuedChunks > m_config.maxChunksQueuedPerUpdate
            ? m_config.maxQueuedChunks - m_config.maxChunksQueuedPerUpdate
            : m_config.maxQueuedChunks;
    if (m_loadedChunks.size() <= targetResident) {
        return;
    }

    struct EvictionCandidate {
        ChunkCoord coord;
        int32_t priority;
    };

    std::vector<EvictionCandidate> candidates;
    candidates.reserve(m_loadedChunks.size());

    const int32_t visibleMinY = cameraChunk.y - RENDER_DISTANCE_VERTICAL_BELOW;
    const int32_t visibleMaxY = cameraChunk.y + RENDER_DISTANCE_VERTICAL_ABOVE;

    for (const auto& [coord, chunk] : m_loadedChunks) {
        if (!chunk || chunk->GetState() == ChunkState::GenerationSubmitted) {
            continue;
        }

        const int32_t dx = coord.x - cameraChunk.x;
        const int32_t dz = coord.z - cameraChunk.z;
        const bool inDenseVisibleWindow =
            std::abs(dx) <= RENDER_DISTANCE_HORIZONTAL &&
            std::abs(dz) <= RENDER_DISTANCE_HORIZONTAL &&
            coord.y >= visibleMinY &&
            coord.y <= visibleMaxY;
        if (inDenseVisibleWindow) {
            continue;
        }

        const int32_t horizontal = std::max(std::abs(dx), std::abs(dz));
        const int32_t vertical =
            coord.y < visibleMinY ? visibleMinY - coord.y :
            coord.y > visibleMaxY ? coord.y - visibleMaxY : 0;
        candidates.push_back(EvictionCandidate{
            coord,
            horizontal * 1024 + vertical * 256 + std::abs(dx) + std::abs(dz)
        });
    }

    if (candidates.empty()) {
        return;
    }

    std::sort(candidates.begin(), candidates.end(), [](const EvictionCandidate& a, const EvictionCandidate& b) {
        return a.priority > b.priority;
    });

    size_t evicted = 0;
    for (const auto& candidate : candidates) {
        if (m_loadedChunks.size() <= targetResident) {
            break;
        }

        auto it = m_loadedChunks.find(candidate.coord);
        if (it == m_loadedChunks.end() || !it->second ||
            it->second->GetState() == ChunkState::GenerationSubmitted) {
            continue;
        }

        if (m_unloadCallback) {
            m_unloadCallback(candidate.coord);
        }

        DeferredChunkDelete deferredDelete;
        deferredDelete.chunk = it->second;
        deferredDelete.fenceValue = m_chunkFenceValue + 10;
        m_deferredChunkDeletes.push_back(deferredDelete);

        m_chunkGenerationFences.erase(candidate.coord);
        m_chunkRequeueCount.erase(candidate.coord);
        m_loadedChunks.erase(it);
        ++evicted;
    }

    if (evicted > 0) {
        static uint32_t trimLogThrottle = 0;
        if (++trimLogThrottle % 30 == 1) {
            spdlog::debug(
                "Trimmed {} non-visible source chunks for VRAM/coverage budget (resident={}, queued={}, budget={})",
                evicted,
                m_loadedChunks.size(),
                m_generationQueue.size(),
                m_config.maxQueuedChunks);
        }
    }
}

bool InfiniteChunkManager::IsWithinLoadWindow(const ChunkCoord& coord, const ChunkCoord& center) const {
    if (coord.y < TERRAIN_CHUNK_MIN_Y || coord.y > TERRAIN_CHUNK_MAX_Y) {
        return false;
    }

    const int32_t dx = coord.x - center.x;
    const int32_t dz = coord.z - center.z;
    const int32_t horizontalDist = std::max(std::abs(dx), std::abs(dz));
    if (horizontalDist > m_config.loadDistanceHorizontal) {
        return false;
    }

    return coord.y >= center.y - m_config.loadDistanceVerticalBelow &&
           coord.y <= center.y + m_config.loadDistanceVerticalAbove;
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
    // Root parameter 0: root constants (ChunkConstants at b0)
    // Root parameter 1: UAV (ChunkVoxelOutput at u0)

    D3D12_ROOT_PARAMETER1 rootParams[2] = {};

    // Parameter 0: root constants
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 8;
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

            spdlog::trace("Deferred chunk delete complete (fence {} >= {})",
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
