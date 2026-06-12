#include "FarVoxelOctree.h"
#include "../Simulation/TerrainConstants.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {
constexpr uint32_t kMaterialSand = 1;
constexpr uint32_t kMaterialWater = 2;
constexpr uint32_t kMaterialStone = 3;
constexpr uint32_t kMaterialDirt = 4;
constexpr uint32_t kLeafFlag = 1;
constexpr uint32_t kInteriorLeafFlag = 2;
// 50 -> 51: the spawn-landmass reshape was added to TerrainHeight (commit 52b108f)
// without bumping the version, so on-disk caches kept serving the OLD flooded-basin
// world (stale tan disc + bright-blue MAT_WATER lakes in aerial views). Bump forces
// a rebuild with the reshape so far-SVO content agrees with the geometry layers.
// 51 -> 52: spawn-land band widened 9300 -> 35000 (continent fills the render
// range). Stale v51 cache would re-introduce the old water-ring archipelago.
// 52 -> 53: band 35000 -> 90000 (TANDEM: at /35000 the band dropped to ~0.80 by
// the 10.4k render edge, so the deepest basins there partial-lift just below sea
// and a 250u low-fly saw that deep-basin-edge water at ~40%; /90000 holds band
// ~0.96 to 10.4k -> solid land across the whole render range at every altitude).
constexpr uint32_t kFarVoxelOctreeCacheVersion = 53;
constexpr uint64_t kMaxFarVoxelOctreeCacheBytes = 512ull * 1024ull * 1024ull;

float Smooth01(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

uint32_t FarTerrainHash3D(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed ^ 2166136261u;
    h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float FarTerrainValueNoise2D(float x, float z, uint32_t seed) {
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t z0 = static_cast<int32_t>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const float sx = Smooth01(fx);
    const float sz = Smooth01(fz);

    auto sample = [seed](int32_t ix, int32_t iz) {
        return static_cast<float>(FarTerrainHash3D(ix, 0, iz, seed) & 0xFFFFFFu) /
            static_cast<float>(0xFFFFFFu);
    };

    const float s00 = sample(x0, z0);
    const float s10 = sample(x0 + 1, z0);
    const float s01 = sample(x0, z0 + 1);
    const float s11 = sample(x0 + 1, z0 + 1);
    const float a = s00 + (s10 - s00) * sx;
    const float b = s01 + (s11 - s01) * sx;
    return (a + (b - a) * sz) * 2.0f - 1.0f;
}

uint32_t CountBits(uint32_t value) {
    uint32_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

bool AddUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool MulUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || (b != 0 && a > std::numeric_limits<uint64_t>::max() / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool FitsSizeT(uint64_t value) {
    return value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool FitsStreamSize(uint64_t value) {
    return value <= static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max());
}

bool IsValidByteRange(uint64_t offset, uint64_t bytes, uint64_t capacity) {
    uint64_t end = 0;
    return AddUint64(offset, bytes, &end) && end <= capacity;
}

bool ComputeFarUploadBytesTotal(
    size_t nodeCount,
    size_t nodeSize,
    size_t pageCount,
    size_t pageSize,
    size_t pageIndexCount,
    size_t pageIndexSize,
    uint64_t* outTotal)
{
    uint64_t nodeBytes = 0;
    uint64_t pageBytes = 0;
    uint64_t pageIndexBytes = 0;
    uint64_t total = 0;
    if (!MulUint64(static_cast<uint64_t>(nodeCount), static_cast<uint64_t>(nodeSize), &nodeBytes) ||
        !MulUint64(static_cast<uint64_t>(pageCount), static_cast<uint64_t>(pageSize), &pageBytes) ||
        !MulUint64(static_cast<uint64_t>(pageIndexCount), static_cast<uint64_t>(pageIndexSize), &pageIndexBytes) ||
        !AddUint64(nodeBytes, pageBytes, &total) ||
        !AddUint64(total, pageIndexBytes, &total)) {
        return false;
    }
    *outTotal = total;
    return true;
}

bool ElementByteCount(uint64_t count, size_t elementSize, uint64_t* outBytes) {
    return MulUint64(count, static_cast<uint64_t>(elementSize), outBytes);
}

bool ReadExact(std::ifstream& stream, void* data, uint64_t bytes) {
    if (bytes == 0) {
        return true;
    }
    if (!data || !FitsStreamSize(bytes)) {
        return false;
    }
    return static_cast<bool>(
        stream.read(
            reinterpret_cast<char*>(data),
            static_cast<std::streamsize>(bytes)));
}

bool WriteExact(std::ofstream& stream, const void* data, uint64_t bytes) {
    if (bytes == 0) {
        return true;
    }
    if (!data || !FitsStreamSize(bytes)) {
        return false;
    }
    return static_cast<bool>(
        stream.write(
            reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(bytes)));
}

bool TryGetInputStreamSize(std::ifstream& stream, uint64_t* outSize) {
    if (!outSize) {
        return false;
    }
    const std::streampos end = stream.tellg();
    if (end < std::streampos{0}) {
        return false;
    }
    const auto size = static_cast<uint64_t>(static_cast<std::streamoff>(end));
    if (size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    *outSize = size;
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream);
}

bool TryFloorDoubleToInt32(double value, int32_t* out) {
    if (!out || !std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(value);
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *out = static_cast<int32_t>(floored);
    return true;
}

struct FarVoxelOctreeCacheHeader {
    uint32_t magic = 0x56534631u; // "VSF1"
    uint32_t version = kFarVoxelOctreeCacheVersion;
    int32_t pageRadius = 0;
    uint32_t maxDepth = 0;
    uint32_t seed = 0;
    float pageSize = 0.0f;
    float rootMinY = 0.0f;
    uint64_t nodeCount = 0;
    uint64_t pageCount = 0;
    uint64_t pageIndexCount = 0;
};

std::filesystem::path FarVoxelOctreeCachePath(const FarVoxelOctreeConfig& config, int32_t radius) {
    return std::filesystem::current_path() /
        ("venpod_far_svo_cache_r" + std::to_string(radius) +
         "_d" + std::to_string(config.maxDepth) +
         "_lf" + std::to_string(static_cast<int32_t>(config.leafFloor)) +
         "_s" + std::to_string(config.seed) + ".bin");
}

}

Result<void> FarVoxelOctree::Initialize(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    const FarVoxelOctreeConfig& config)
{
    if (!device) {
        return Error("FarVoxelOctree::Initialize - device is null");
    }

    Shutdown();
    BuildResult built = BuildCpuData(config);
    if (!built.success) {
        return Error("{}", built.error);
    }

    m_config = built.config;
    m_stats = built.stats;
    m_nodes = std::move(built.nodes);
    m_pages = std::move(built.pages);
    m_pageIndex = std::move(built.pageIndex);
    return UploadToGpu(device, heapManager);
}

void FarVoxelOctree::BeginAsyncLoad(const FarVoxelOctreeConfig& config) {
    if (m_asyncPending) {
        return;
    }
    Shutdown();
    m_asyncPending = true;
    m_asyncBuild = std::async(std::launch::async, [config]() {
        return FarVoxelOctree::BuildCpuData(config);
    });
}

bool FarVoxelOctree::HasCompatibleCache(const FarVoxelOctreeConfig& config) {
    std::string validationError;
    if (!ValidateFarVoxelOctreeConfigForBuild(config, &validationError)) {
        return false;
    }
    const int32_t radius = std::max(1, config.pageRadius);
    uint64_t pageCountPerAxis64 = 0;
    uint64_t pageIndexCount64 = 0;
    if (!AddUint64(static_cast<uint64_t>(radius), static_cast<uint64_t>(radius), &pageCountPerAxis64) ||
        !AddUint64(pageCountPerAxis64, 1u, &pageCountPerAxis64) ||
        !MulUint64(pageCountPerAxis64, pageCountPerAxis64, &pageIndexCount64)) {
        return false;
    }

    const auto cachePath = FarVoxelOctreeCachePath(config, radius);
    std::ifstream in(cachePath, std::ios::binary | std::ios::ate);
    uint64_t cacheBytes = 0;
    FarVoxelOctreeCacheHeader header{};
    if (!TryGetInputStreamSize(in, &cacheBytes) ||
        cacheBytes < static_cast<uint64_t>(sizeof(header)) ||
        cacheBytes > kMaxFarVoxelOctreeCacheBytes ||
        !in.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.magic != 0x56534631u ||
        header.version != kFarVoxelOctreeCacheVersion ||
        header.pageRadius != radius ||
        header.maxDepth != config.maxDepth ||
        header.seed != config.seed ||
        header.pageSize != config.pageSize ||
        header.rootMinY != config.rootMinY ||
        header.nodeCount == 0 ||
        header.pageCount == 0 ||
        header.pageIndexCount != pageIndexCount64 ||
        !FitsSizeT(header.nodeCount) ||
        !FitsSizeT(header.pageCount) ||
        !FitsSizeT(header.pageIndexCount)) {
        return false;
    }

    uint64_t nodeBytes = 0;
    uint64_t pageBytes = 0;
    uint64_t pageIndexBytes = 0;
    uint64_t payloadBytes = 0;
    uint64_t expectedCacheBytes = 0;
    return ElementByteCount(header.nodeCount, sizeof(Node), &nodeBytes) &&
        ElementByteCount(header.pageCount, sizeof(Page), &pageBytes) &&
        ElementByteCount(header.pageIndexCount, sizeof(uint32_t), &pageIndexBytes) &&
        AddUint64(nodeBytes, pageBytes, &payloadBytes) &&
        AddUint64(payloadBytes, pageIndexBytes, &payloadBytes) &&
        AddUint64(static_cast<uint64_t>(sizeof(header)), payloadBytes, &expectedCacheBytes) &&
        expectedCacheBytes == cacheBytes;
}

bool FarVoxelOctree::IsGpuUploadPending() const {
    return m_gpuUploadStage != GpuUploadStage::Idle &&
           m_gpuUploadStage != GpuUploadStage::Complete;
}

bool FarVoxelOctree::HasPendingGpuUploadCopies() const {
    return !m_pendingGpuCopies.empty();
}

bool FarVoxelOctree::EmitPendingGpuUploadCopies(ID3D12GraphicsCommandList* commandList) {
    if (!commandList || m_pendingGpuCopies.empty()) {
        return false;
    }

    bool copiedAny = false;
    for (const PendingGpuCopy& copy : m_pendingGpuCopies) {
        GPUBuffer* dst = nullptr;
        UploadBuffer* src = nullptr;
        switch (copy.stage) {
        case GpuUploadStage::Nodes:
            dst = &m_nodeBuffer;
            src = &m_nodeUploadBuffer;
            break;
        case GpuUploadStage::Pages:
            dst = &m_pageBuffer;
            src = &m_pageUploadBuffer;
            break;
        case GpuUploadStage::PageIndex:
            dst = &m_pageIndexBuffer;
            src = &m_pageIndexUploadBuffer;
            break;
        case GpuUploadStage::Idle:
        case GpuUploadStage::Complete:
            break;
        }
        if (!dst || !src || !dst->GetResource() || !src->GetResource() || copy.bytes == 0) {
            continue;
        }
        if (!IsValidByteRange(copy.offset, copy.bytes, dst->GetSize()) ||
            !IsValidByteRange(copy.offset, copy.bytes, src->GetSize())) {
            spdlog::warn(
                "Far octree skipped invalid pending GPU upload copy: stage={} offset={} bytes={} dstSize={} srcSize={}",
                static_cast<uint32_t>(copy.stage),
                copy.offset,
                copy.bytes,
                dst->GetSize(),
                src->GetSize());
            m_pendingGpuCopies.clear();
            return false;
        }

        dst->TransitionTo(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            dst->GetResource(),
            copy.offset,
            src->GetResource(),
            copy.offset,
            copy.bytes);
        copiedAny = true;
    }

    if (m_nodeBuffer.GetResource()) {
        m_nodeBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (m_pageBuffer.GetResource()) {
        m_pageBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (m_pageIndexBuffer.GetResource()) {
        m_pageIndexBuffer.TransitionTo(
            commandList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_pendingGpuCopies.clear();
    return copiedAny;
}

const char* FarVoxelOctree::GetGpuUploadStageName() const {
    switch (m_gpuUploadStage) {
    case GpuUploadStage::Idle: return "idle";
    case GpuUploadStage::Nodes: return "nodes";
    case GpuUploadStage::Pages: return "pages";
    case GpuUploadStage::PageIndex: return "page-index";
    case GpuUploadStage::Complete: return "complete";
    }
    return "unknown";
}

bool FarVoxelOctree::TryFinalizeAsyncUpload(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    uint64_t maxUploadBytes,
    uint32_t maxCpuWaitMs)
{
    if (!m_asyncPending || !m_asyncBuild.valid()) {
        if (IsGpuUploadPending()) {
            return PumpGpuUpload(device, heapManager, maxUploadBytes);
        }
        return IsValid();
    }

    const auto waitDuration = maxCpuWaitMs > 0u
        ? std::chrono::milliseconds(maxCpuWaitMs)
        : std::chrono::milliseconds(0);
    if (m_asyncBuild.wait_for(waitDuration) != std::future_status::ready) {
        return false;
    }

    BuildResult built = m_asyncBuild.get();
    m_asyncPending = false;
    if (!built.success) {
        spdlog::warn("Far sparse voxel octree async load failed: {}", built.error);
        return false;
    }

    m_config = built.config;
    m_stats = built.stats;
    m_nodes = std::move(built.nodes);
    m_pages = std::move(built.pages);
    m_pageIndex = std::move(built.pageIndex);
    uint64_t uploadBytesTotal = 0;
    if (!ComputeFarUploadBytesTotal(
            m_nodes.size(),
            sizeof(Node),
            m_pages.size(),
            sizeof(Page),
            m_pageIndex.size(),
            sizeof(uint32_t),
            &uploadBytesTotal)) {
        spdlog::warn("Far sparse voxel octree async upload size overflow");
        return false;
    }
    m_stats.gpuUploadBytesTotal = uploadBytesTotal;
    m_stats.gpuUploadBytesUploaded = 0;
    m_stats.gpuUploadStageBytesTotal = 0;
    m_stats.gpuUploadStageBytesUploaded = 0;
    m_stats.gpuUploadMs = 0.0;
    m_gpuUploadStage = GpuUploadStage::Nodes;
    m_gpuUploadStageOffset = 0;

    return PumpGpuUpload(device, heapManager, maxUploadBytes);
}

FarVoxelOctree::BuildResult FarVoxelOctree::BuildCpuData(const FarVoxelOctreeConfig& config) {
    const auto buildStart = std::chrono::steady_clock::now();
    BuildResult result;
    result.config = config;

    FarVoxelOctree builder;
    builder.m_config = config;

    const int32_t radius = std::max(1, builder.m_config.pageRadius);
    if (!ValidateFarVoxelOctreeConfigForBuild(builder.m_config, &result.error)) {
        return result;
    }
    uint64_t pageCountPerAxis64 = 0;
    uint64_t pageIndexCount64 = 0;
    if (!AddUint64(static_cast<uint64_t>(radius), static_cast<uint64_t>(radius), &pageCountPerAxis64) ||
        !AddUint64(pageCountPerAxis64, 1u, &pageCountPerAxis64) ||
        !MulUint64(pageCountPerAxis64, pageCountPerAxis64, &pageIndexCount64) ||
        pageCountPerAxis64 > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        pageIndexCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
        !FitsSizeT(pageIndexCount64)) {
        result.error = "FarVoxelOctree page grid is too large";
        return result;
    }
    const int32_t pageCountPerAxis = static_cast<int32_t>(pageCountPerAxis64);
    constexpr uint32_t kMissingPage = 0xFFFFFFFFu;

    const auto cachePath = FarVoxelOctreeCachePath(builder.m_config, radius);

    bool loadedFromCache = false;
    {
        std::ifstream in(cachePath, std::ios::binary | std::ios::ate);
        uint64_t cacheBytes = 0;
        FarVoxelOctreeCacheHeader header{};
        if (TryGetInputStreamSize(in, &cacheBytes) &&
            cacheBytes >= static_cast<uint64_t>(sizeof(header)) &&
            cacheBytes <= kMaxFarVoxelOctreeCacheBytes &&
            in.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == 0x56534631u &&
            header.version == kFarVoxelOctreeCacheVersion &&
            header.pageRadius == radius &&
            header.maxDepth == builder.m_config.maxDepth &&
            header.seed == builder.m_config.seed &&
            header.pageSize == builder.m_config.pageSize &&
            header.rootMinY == builder.m_config.rootMinY &&
            header.nodeCount > 0 &&
            header.pageCount > 0 &&
            header.pageIndexCount == pageIndexCount64 &&
            FitsSizeT(header.nodeCount) &&
            FitsSizeT(header.pageCount) &&
            FitsSizeT(header.pageIndexCount)) {
            uint64_t nodeBytes = 0;
            uint64_t pageBytes = 0;
            uint64_t pageIndexBytes = 0;
            uint64_t payloadBytes = 0;
            uint64_t expectedCacheBytes = 0;
            if (ElementByteCount(header.nodeCount, sizeof(Node), &nodeBytes) &&
                ElementByteCount(header.pageCount, sizeof(Page), &pageBytes) &&
                ElementByteCount(header.pageIndexCount, sizeof(uint32_t), &pageIndexBytes) &&
                AddUint64(nodeBytes, pageBytes, &payloadBytes) &&
                AddUint64(payloadBytes, pageIndexBytes, &payloadBytes) &&
                AddUint64(static_cast<uint64_t>(sizeof(header)), payloadBytes, &expectedCacheBytes) &&
                expectedCacheBytes == cacheBytes &&
                FitsStreamSize(nodeBytes) &&
                FitsStreamSize(pageBytes) &&
                FitsStreamSize(pageIndexBytes)) {
                builder.m_nodes.resize(static_cast<size_t>(header.nodeCount));
                builder.m_pages.resize(static_cast<size_t>(header.pageCount));
                builder.m_pageIndex.resize(static_cast<size_t>(header.pageIndexCount));

                if (ReadExact(in, builder.m_nodes.data(), nodeBytes) &&
                    ReadExact(in, builder.m_pages.data(), pageBytes) &&
                    ReadExact(in, builder.m_pageIndex.data(), pageIndexBytes)) {
                    loadedFromCache = true;
                } else {
                    builder.m_nodes.clear();
                    builder.m_pages.clear();
                    builder.m_pageIndex.clear();
                }
            }
        }
    }

    if (!loadedFromCache) {
        uint64_t reserveNodeCount = 0;
        if (!MulUint64(pageIndexCount64, 64u, &reserveNodeCount) ||
            !FitsSizeT(reserveNodeCount)) {
            result.error = "FarVoxelOctree node reserve is too large";
            return result;
        }
        builder.m_nodes.reserve(static_cast<size_t>(reserveNodeCount));
        builder.m_pages.reserve(static_cast<size_t>(pageIndexCount64));
        builder.m_pageIndex.assign(static_cast<size_t>(pageIndexCount64), kMissingPage);

        struct PendingPageBuild {
            BuildBounds rootBounds{};
            Page page{};
            size_t denseIndex = 0;
        };
        struct CompletedPageBuild {
            Page page{};
            size_t denseIndex = 0;
            std::vector<Node> nodes;
            bool success = false;
        };
        std::vector<PendingPageBuild> pendingPages;
        pendingPages.reserve(static_cast<size_t>(pageIndexCount64));

        for (int32_t pz = -radius; pz <= radius; ++pz) {
            for (int32_t px = -radius; px <= radius; ++px) {
                const double originXDouble =
                    static_cast<double>(px) * static_cast<double>(builder.m_config.pageSize);
                const double originZDouble =
                    static_cast<double>(pz) * static_cast<double>(builder.m_config.pageSize);
                int32_t pageOriginX = 0;
                int32_t pageOriginY = 0;
                int32_t pageOriginZ = 0;
                if (!TryFloorDoubleToInt32(originXDouble, &pageOriginX) ||
                    !TryFloorDoubleToInt32(static_cast<double>(builder.m_config.rootMinY), &pageOriginY) ||
                    !TryFloorDoubleToInt32(originZDouble, &pageOriginZ)) {
                    result.error = "FarVoxelOctree page origin exceeded int32 world range";
                    return result;
                }
                const float originX = static_cast<float>(originXDouble);
                const float originZ = static_cast<float>(originZDouble);
                const BuildBounds rootBounds{
                    originX,
                    builder.m_config.rootMinY,
                    originZ,
                    builder.m_config.pageSize
                };

                if (!builder.CellMayContainTerrain(rootBounds)) {
                    continue;
                }

                Page page;
                page.originX = pageOriginX;
                page.originY = pageOriginY;
                page.originZ = pageOriginZ;
                const size_t denseIndex = static_cast<size_t>(pz + radius) *
                                          static_cast<size_t>(pageCountPerAxis) +
                                          static_cast<size_t>(px + radius);
                pendingPages.push_back(PendingPageBuild{rootBounds, page, denseIndex});
            }
        }

        std::vector<CompletedPageBuild> completedPages(pendingPages.size());
        if (!pendingPages.empty()) {
            const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
            const uint32_t workerCount = std::clamp<uint32_t>(
                hardwareThreads,
                1u,
                static_cast<uint32_t>(pendingPages.size()));
            std::atomic<size_t> nextPage{0};
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (uint32_t worker = 0; worker < workerCount; ++worker) {
                workers.emplace_back([&]() {
                    for (;;) {
                        const size_t pageBuildIndex = nextPage.fetch_add(1, std::memory_order_relaxed);
                        if (pageBuildIndex >= pendingPages.size()) {
                            break;
                        }
                        const PendingPageBuild& pending = pendingPages[pageBuildIndex];
                        FarVoxelOctree pageBuilder;
                        pageBuilder.m_config = builder.m_config;
                        pageBuilder.m_nodes.reserve(32768u);
                        const uint32_t rootNode = pageBuilder.BuildNode(pending.rootBounds, 0);

                        CompletedPageBuild completed;
                        completed.page = pending.page;
                        completed.page.rootNode = rootNode;
                        completed.denseIndex = pending.denseIndex;
                        completed.nodes = std::move(pageBuilder.m_nodes);
                        completed.success = !completed.nodes.empty();
                        completedPages[pageBuildIndex] = std::move(completed);
                    }
                });
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
        }

        for (CompletedPageBuild& completed : completedPages) {
            if (!completed.success || completed.nodes.empty()) {
                continue;
            }
            if (builder.m_pages.size() >= std::numeric_limits<uint32_t>::max() ||
                builder.m_nodes.size() >= std::numeric_limits<uint32_t>::max()) {
                result.error = "FarVoxelOctree generated counts exceed runtime index limits";
                return result;
            }
            const uint32_t nodeBase = static_cast<uint32_t>(builder.m_nodes.size());
            for (Node& node : completed.nodes) {
                if ((node.flags & kLeafFlag) == 0u && node.childBase != 0xFFFFFFFFu) {
                    node.childBase += nodeBase;
                }
            }
            completed.page.rootNode += nodeBase;
            const uint32_t pageIndex = static_cast<uint32_t>(builder.m_pages.size());
            builder.m_pageIndex[completed.denseIndex] = pageIndex;
            builder.m_nodes.insert(
                builder.m_nodes.end(),
                std::make_move_iterator(completed.nodes.begin()),
                std::make_move_iterator(completed.nodes.end()));
            builder.m_pages.push_back(completed.page);
        }

        FarVoxelOctreeCacheHeader header{};
        header.pageRadius = radius;
        header.maxDepth = builder.m_config.maxDepth;
        header.seed = builder.m_config.seed;
        header.pageSize = builder.m_config.pageSize;
        header.rootMinY = builder.m_config.rootMinY;
        header.nodeCount = static_cast<uint64_t>(builder.m_nodes.size());
        header.pageCount = static_cast<uint64_t>(builder.m_pages.size());
        header.pageIndexCount = static_cast<uint64_t>(builder.m_pageIndex.size());

        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        uint64_t nodeBytes = 0;
        uint64_t pageBytes = 0;
        uint64_t pageIndexBytes = 0;
        if (ElementByteCount(builder.m_nodes.size(), sizeof(Node), &nodeBytes) &&
            ElementByteCount(builder.m_pages.size(), sizeof(Page), &pageBytes) &&
            ElementByteCount(builder.m_pageIndex.size(), sizeof(uint32_t), &pageIndexBytes) &&
            WriteExact(out, &header, sizeof(header)) &&
            WriteExact(out, builder.m_nodes.data(), nodeBytes) &&
            WriteExact(out, builder.m_pages.data(), pageBytes) &&
            WriteExact(out, builder.m_pageIndex.data(), pageIndexBytes)) {
            spdlog::info("Saved far voxel octree cache: {}", cachePath.string());
        }
    }

    if (builder.m_nodes.empty() || builder.m_pages.empty()) {
        result.error = "FarVoxelOctree generated no nodes/pages";
        return result;
    }
    if (builder.m_nodes.size() > std::numeric_limits<uint32_t>::max() ||
        builder.m_pages.size() > std::numeric_limits<uint32_t>::max() ||
        builder.m_pageIndex.size() > std::numeric_limits<uint32_t>::max()) {
        result.error = "FarVoxelOctree generated counts exceed runtime stats limits";
        return result;
    }

    result.stats.pageCount = static_cast<uint32_t>(builder.m_pages.size());
    result.stats.nodeCount = static_cast<uint32_t>(builder.m_nodes.size());
    result.stats.pageIndexCount = static_cast<uint32_t>(builder.m_pageIndex.size());

    // [P1 SVDAG materialization] Bottom-up de-duplicate identical subtrees into a real
    // pointer-indirection DAG (the structure the new separate raymarch pass will consume).
    // Format: DagNode{childPtrBase, childMask, material, flags} (16B, fixed-stride so a node
    // is addressable by logical id) + a flat ChildPointers array. child c's node index =
    // ChildPointers[childPtrBase + countbits(childMask & ((1<<c)-1))]. Two parents sharing a
    // subtree both point their ChildPointers entry at the SAME DagNode id -> true DAG sharing
    // (impossible in the contiguous-child tree). Leaves carry material/flags, no pointer block.
    // Gated so normal runs (rebrun.ps1, env unset) pay nothing and behave byte-identically.
    if (const char* analyze = std::getenv("VENPOD_FARVOXEL_DAG_ANALYZE");
        analyze && analyze[0] == '1' && !builder.m_nodes.empty()) {
        const auto matStart = std::chrono::steady_clock::now();
        const size_t nodeTotal = builder.m_nodes.size();

        struct DagNode {
            uint32_t childPtrBase;
            uint32_t childMask;
            uint32_t material;
            uint32_t flags;
        };
        std::vector<DagNode> dagNodes;
        std::vector<uint32_t> childPointers;
        dagNodes.reserve(nodeTotal / 16 + 16);
        childPointers.reserve(nodeTotal / 8 + 16);

        std::vector<uint32_t> canon(nodeTotal, 0xFFFFFFFFu);  // tree index -> DAG node id
        std::unordered_map<std::string, uint32_t> uniqueMap;
        uniqueMap.reserve(nodeTotal / 2 + 16);
        std::string key;
        key.reserve(48);
        auto put = [&key](uint32_t value) {
            key.push_back(static_cast<char>(value & 0xFFu));
            key.push_back(static_cast<char>((value >> 8) & 0xFFu));
            key.push_back(static_cast<char>((value >> 16) & 0xFFu));
            key.push_back(static_cast<char>((value >> 24) & 0xFFu));
        };
        // Children always sit at higher indices than their parent within a page block,
        // so descending index order canonicalizes children before their parents.
        for (size_t reverse = 0; reverse < nodeTotal; ++reverse) {
            const size_t index = nodeTotal - 1 - reverse;
            const Node& node = builder.m_nodes[index];
            const bool leaf = (node.flags & kLeafFlag) != 0u ||
                              node.childMask == 0u ||
                              node.childBase == 0xFFFFFFFFu;
            key.clear();
            uint32_t childDagIds[8];
            uint32_t childDagCount = 0;
            if (leaf) {
                put(0xA11Eu);
                put(node.flags & (kLeafFlag | kInteriorLeafFlag));
                put(node.material);
            } else {
                put(0x1u);
                put(node.childMask);
                uint32_t compact = 0;
                for (uint32_t child = 0; child < 8; ++child) {
                    if ((node.childMask & (1u << child)) == 0u) {
                        continue;
                    }
                    const uint32_t childIndex = node.childBase + compact;
                    ++compact;
                    const uint32_t childDagId =
                        (childIndex < nodeTotal) ? canon[childIndex] : 0xFFFFFFFFu;
                    childDagIds[childDagCount++] = childDagId;
                    put(childDagId);
                }
            }
            auto found = uniqueMap.find(key);
            if (found != uniqueMap.end()) {
                canon[index] = found->second;
                continue;
            }
            const uint32_t newId = static_cast<uint32_t>(dagNodes.size());
            DagNode dag{};
            dag.material = node.material;
            dag.flags = node.flags;
            if (leaf) {
                dag.childMask = 0u;
                dag.childPtrBase = 0xFFFFFFFFu;
            } else {
                dag.childMask = node.childMask;
                dag.childPtrBase = static_cast<uint32_t>(childPointers.size());
                for (uint32_t k = 0; k < childDagCount; ++k) {
                    childPointers.push_back(childDagIds[k]);
                }
            }
            dagNodes.push_back(dag);
            uniqueMap.emplace(key, newId);
            canon[index] = newId;
        }

        // Structural integrity: every internal node's pointer block is in-bounds and every
        // pointer references a valid DAG node.
        bool structureOk = true;
        for (size_t i = 0; i < dagNodes.size() && structureOk; ++i) {
            const DagNode& dag = dagNodes[i];
            const bool leaf = (dag.flags & kLeafFlag) != 0u ||
                              dag.childMask == 0u ||
                              dag.childPtrBase == 0xFFFFFFFFu;
            if (leaf) {
                continue;
            }
            const uint32_t count = CountBits(dag.childMask);
            if (static_cast<uint64_t>(dag.childPtrBase) + count > childPointers.size()) {
                structureOk = false;
                break;
            }
            for (uint32_t k = 0; k < count; ++k) {
                if (childPointers[dag.childPtrBase + k] >= dagNodes.size()) {
                    structureOk = false;
                    break;
                }
            }
        }

        // Correctness: re-expand the DAG and the tree in lockstep from a page root; every
        // node must agree (leaf-ness, childMask, material, flags). Bounded visit budget keeps
        // the one-time gated check cheap while still touching real terrain subtrees.
        bool verifyOk = structureOk;
        uint64_t verifyVisited = 0;
        const uint64_t verifyBudget = 4'000'000;
        if (verifyOk && !builder.m_pages.empty()) {
            std::vector<std::pair<uint32_t, uint32_t>> stack;  // (treeIndex, dagId)
            for (size_t p = 0; p < builder.m_pages.size() && p < 8 && verifyOk; ++p) {
                const uint32_t treeRoot = builder.m_pages[p].rootNode;
                if (treeRoot >= nodeTotal) {
                    verifyOk = false;
                    break;
                }
                stack.clear();
                stack.emplace_back(treeRoot, canon[treeRoot]);
                while (!stack.empty() && verifyVisited < verifyBudget) {
                    const auto pair = stack.back();
                    stack.pop_back();
                    ++verifyVisited;
                    const uint32_t treeIdx = pair.first;
                    const uint32_t dagId = pair.second;
                    if (treeIdx >= nodeTotal || dagId >= dagNodes.size()) {
                        verifyOk = false;
                        break;
                    }
                    const Node& tn = builder.m_nodes[treeIdx];
                    const DagNode& dn = dagNodes[dagId];
                    const bool tLeaf = (tn.flags & kLeafFlag) != 0u ||
                                       tn.childMask == 0u ||
                                       tn.childBase == 0xFFFFFFFFu;
                    const bool dLeaf = (dn.flags & kLeafFlag) != 0u ||
                                       dn.childMask == 0u ||
                                       dn.childPtrBase == 0xFFFFFFFFu;
                    if (tLeaf != dLeaf) {
                        verifyOk = false;
                        break;
                    }
                    if (tLeaf) {
                        if (tn.material != dn.material ||
                            (tn.flags & (kLeafFlag | kInteriorLeafFlag)) !=
                                (dn.flags & (kLeafFlag | kInteriorLeafFlag))) {
                            verifyOk = false;
                            break;
                        }
                        continue;
                    }
                    if (tn.childMask != dn.childMask || tn.material != dn.material) {
                        verifyOk = false;
                        break;
                    }
                    uint32_t compact = 0;
                    for (uint32_t child = 0; child < 8; ++child) {
                        if ((tn.childMask & (1u << child)) == 0u) {
                            continue;
                        }
                        const uint32_t treeChild = tn.childBase + compact;
                        const uint32_t dagChild = childPointers[dn.childPtrBase + compact];
                        ++compact;
                        stack.emplace_back(treeChild, dagChild);
                    }
                }
            }
        }

        const double matMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - matStart).count();
        const size_t dagNodeCount = dagNodes.size();
        const double ratio = dagNodeCount > 0
            ? static_cast<double>(nodeTotal) / static_cast<double>(dagNodeCount)
            : 0.0;
        const double treeMiB = static_cast<double>(nodeTotal * sizeof(Node)) / (1024.0 * 1024.0);
        const double dagMiB = static_cast<double>(
            dagNodeCount * sizeof(DagNode) + childPointers.size() * sizeof(uint32_t)) /
            (1024.0 * 1024.0);
        spdlog::info(
            "[DAG-MATERIALIZE] maxDepth={} leafFloor=16 pages={} : tree nodes={} ({:.1f} MiB) -> "
            "DAG nodes={} ptrs={} ({:.2f}x, {:.1f} MiB) verify={} (visited {}) in {:.1f} ms",
            builder.m_config.maxDepth, builder.m_pages.size(), nodeTotal, treeMiB,
            dagNodeCount, childPointers.size(), ratio, dagMiB,
            verifyOk ? "OK" : "FAIL", verifyVisited, matMs);
    }

    result.stats.pageRadius = radius;
    result.stats.maxDepth = builder.m_config.maxDepth;
    result.stats.pageSize = builder.m_config.pageSize;
    result.stats.rootMinY = builder.m_config.rootMinY;
    result.stats.coveredWorldSize = static_cast<float>(pageCountPerAxis) * builder.m_config.pageSize;
    result.stats.loadedFromCache = loadedFromCache;
    result.stats.cpuBuildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - buildStart).count();
    result.nodes = std::move(builder.m_nodes);
    result.pages = std::move(builder.m_pages);
    result.pageIndex = std::move(builder.m_pageIndex);
    result.success = true;
    return result;
}

Result<void> FarVoxelOctree::UploadToGpu(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    if (!device) {
        return Error("FarVoxelOctree::UploadToGpu - device is null");
    }
    if (m_nodes.empty() || m_pages.empty() || m_pageIndex.empty()) {
        return Error("FarVoxelOctree::UploadToGpu - CPU data is empty");
    }

    uint64_t uploadBytesTotal = 0;
    if (!ComputeFarUploadBytesTotal(
            m_nodes.size(),
            sizeof(Node),
            m_pages.size(),
            sizeof(Page),
            m_pageIndex.size(),
            sizeof(uint32_t),
            &uploadBytesTotal)) {
        return Error("FarVoxelOctree::UploadToGpu - upload size overflow");
    }
    m_stats.gpuUploadBytesTotal = uploadBytesTotal;
    m_stats.gpuUploadBytesUploaded = 0;
    m_stats.gpuUploadStageBytesTotal = 0;
    m_stats.gpuUploadStageBytesUploaded = 0;
    m_stats.gpuUploadMs = 0.0;
    m_gpuUploadStage = GpuUploadStage::Nodes;
    m_gpuUploadStageOffset = 0;
    if (!PumpGpuUpload(device, heapManager, UINT64_MAX)) {
        return Error("FarVoxelOctree::UploadToGpu - staged upload did not complete");
    }
    return {};
}

bool FarVoxelOctree::PumpGpuUpload(
    ID3D12Device* device,
    DescriptorHeapManager& heapManager,
    uint64_t maxUploadBytes)
{
    if (!device || m_nodes.empty() || m_pages.empty() || m_pageIndex.empty()) {
        return false;
    }
    if (m_gpuUploadStage == GpuUploadStage::Complete) {
        return IsValid();
    }
    if (m_gpuUploadStage == GpuUploadStage::Idle) {
        m_gpuUploadStage = GpuUploadStage::Nodes;
        m_gpuUploadStageOffset = 0;
    }
    if (maxUploadBytes == 0) {
        return false;
    }

    const auto uploadStart = std::chrono::steady_clock::now();
    uint64_t budgetRemaining = maxUploadBytes;
    m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
    const auto copyStage = [&](
        GPUBuffer& buffer,
        UploadBuffer& uploadBuffer,
        const void* source,
        uint64_t totalBytes,
        uint32_t stride,
        const char* name) -> bool {
        if (!buffer.GetResource()) {
            auto result = buffer.Initialize(
                device,
                totalBytes,
                BufferUsage::Default | BufferUsage::StructuredBuffer,
                stride,
                name);
            if (!result) {
                spdlog::warn("Failed to create far octree default buffer {}: {}", name, result.error());
                return false;
            }
        }
        if (!uploadBuffer.GetResource()) {
            const std::string uploadName = std::string(name) + "_Upload";
            auto result = uploadBuffer.Initialize(device, totalBytes, uploadName.c_str());
            if (!result) {
                spdlog::warn("Failed to create far octree staging buffer {}: {}", uploadName, result.error());
                return false;
            }
        }

        m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
        m_stats.gpuUploadStageBytesTotal = totalBytes;
        m_stats.gpuUploadStageBytesUploaded = m_gpuUploadStageOffset;
        if (!source ||
            m_gpuUploadStageOffset > totalBytes ||
            totalBytes > buffer.GetSize() ||
            totalBytes > uploadBuffer.GetSize()) {
            spdlog::warn(
                "Far octree upload stage {} has invalid bounds: offset={} total={} dstSize={} uploadSize={}",
                name,
                m_gpuUploadStageOffset,
                totalBytes,
                buffer.GetSize(),
                uploadBuffer.GetSize());
            m_gpuUploadStage = GpuUploadStage::Idle;
            return false;
        }
        const uint64_t remaining = totalBytes - m_gpuUploadStageOffset;
        const uint64_t bytesToCopy = std::min(remaining, budgetRemaining);
        if (bytesToCopy > 0) {
            if (bytesToCopy > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
                !IsValidByteRange(m_gpuUploadStageOffset, bytesToCopy, totalBytes)) {
                spdlog::warn(
                    "Far octree upload stage {} rejected invalid copy range: offset={} bytes={} total={}",
                    name,
                    m_gpuUploadStageOffset,
                    bytesToCopy,
                    totalBytes);
                m_gpuUploadStage = GpuUploadStage::Idle;
                return false;
            }
            const uint8_t* sourceBytes = static_cast<const uint8_t*>(source);
            uint8_t* mappedUpload = static_cast<uint8_t*>(uploadBuffer.GetMappedData());
            if (!mappedUpload) {
                spdlog::warn("Far octree staging buffer {} is not mapped", name);
                return false;
            }
            std::memcpy(mappedUpload + m_gpuUploadStageOffset, sourceBytes + m_gpuUploadStageOffset, static_cast<size_t>(bytesToCopy));
            m_pendingGpuCopies.push_back(PendingGpuCopy{
                m_gpuUploadStage,
                m_gpuUploadStageOffset,
                bytesToCopy
            });
            m_gpuUploadStageOffset += bytesToCopy;
            uint64_t uploadedBytes = 0;
            if (!AddUint64(m_stats.gpuUploadBytesUploaded, bytesToCopy, &uploadedBytes)) {
                spdlog::warn("Far octree upload byte counter overflow in stage {}", name);
                m_gpuUploadStage = GpuUploadStage::Idle;
                return false;
            }
            m_stats.gpuUploadBytesUploaded = std::min(uploadedBytes, m_stats.gpuUploadBytesTotal);
            m_stats.gpuUploadStageBytesUploaded = m_gpuUploadStageOffset;
            budgetRemaining -= bytesToCopy;
        }
        return m_gpuUploadStageOffset == totalBytes;
    };

    while (budgetRemaining > 0 && m_gpuUploadStage != GpuUploadStage::Complete) {
        switch (m_gpuUploadStage) {
        case GpuUploadStage::Nodes: {
            const uint64_t bytes = static_cast<uint64_t>(m_nodes.size()) * sizeof(Node);
            if (!copyStage(m_nodeBuffer, m_nodeUploadBuffer, m_nodes.data(), bytes, sizeof(Node), "FarVoxelOctree_Nodes")) {
                budgetRemaining = 0;
                break;
            }
            auto srvResult = m_nodeBuffer.CreateSRV(device, heapManager);
            if (!srvResult) {
                spdlog::warn("Failed to create far octree node SRV: {}", srvResult.error());
                m_gpuUploadStage = GpuUploadStage::Idle;
                return false;
            }
            m_gpuUploadStage = GpuUploadStage::Pages;
            m_gpuUploadStageOffset = 0;
            m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
            m_stats.gpuUploadStageBytesTotal = 0;
            m_stats.gpuUploadStageBytesUploaded = 0;
            break;
        }
        case GpuUploadStage::Pages: {
            const uint64_t bytes = static_cast<uint64_t>(m_pages.size()) * sizeof(Page);
            if (!copyStage(m_pageBuffer, m_pageUploadBuffer, m_pages.data(), bytes, sizeof(Page), "FarVoxelOctree_Pages")) {
                budgetRemaining = 0;
                break;
            }
            auto srvResult = m_pageBuffer.CreateSRV(device, heapManager);
            if (!srvResult) {
                spdlog::warn("Failed to create far octree page SRV: {}", srvResult.error());
                m_gpuUploadStage = GpuUploadStage::Idle;
                return false;
            }
            m_gpuUploadStage = GpuUploadStage::PageIndex;
            m_gpuUploadStageOffset = 0;
            m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
            m_stats.gpuUploadStageBytesTotal = 0;
            m_stats.gpuUploadStageBytesUploaded = 0;
            break;
        }
        case GpuUploadStage::PageIndex: {
            const uint64_t bytes = static_cast<uint64_t>(m_pageIndex.size()) * sizeof(uint32_t);
            if (!copyStage(m_pageIndexBuffer, m_pageIndexUploadBuffer, m_pageIndex.data(), bytes, sizeof(uint32_t), "FarVoxelOctree_PageIndex")) {
                budgetRemaining = 0;
                break;
            }
            auto srvResult = m_pageIndexBuffer.CreateSRV(device, heapManager);
            if (!srvResult) {
                spdlog::warn("Failed to create far octree page index SRV: {}", srvResult.error());
                m_gpuUploadStage = GpuUploadStage::Idle;
                return false;
            }
            m_gpuUploadStage = GpuUploadStage::Complete;
            m_gpuUploadStageOffset = 0;
            m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
            m_stats.gpuUploadStageBytesTotal = 0;
            m_stats.gpuUploadStageBytesUploaded = 0;
            break;
        }
        case GpuUploadStage::Idle:
        case GpuUploadStage::Complete:
            m_gpuUploadStage = GpuUploadStage::Complete;
            m_stats.gpuUploadStage = static_cast<uint32_t>(m_gpuUploadStage);
            break;
        }
    }

    m_stats.gpuUploadMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - uploadStart).count();
    if (m_gpuUploadStage == GpuUploadStage::Complete) {
        spdlog::info("Far voxel octree initialized: {} pages, {} nodes, {} page-index cells, pageSize={}, coverage={} world units, source={}, {:.1f} ms",
            m_stats.pageCount,
            m_stats.nodeCount,
            m_stats.pageIndexCount,
            m_stats.pageSize,
            m_stats.coveredWorldSize,
            m_stats.loadedFromCache ? "cache" : "build",
            m_stats.cpuBuildMs + m_stats.gpuUploadMs);
        return IsValid();
    }
    return false;
}

void FarVoxelOctree::Shutdown() {
    if (m_asyncPending && m_asyncBuild.valid() &&
        m_asyncBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        (void)m_asyncBuild.get();
        m_asyncPending = false;
    }
    m_nodeBuffer.Shutdown();
    m_pageBuffer.Shutdown();
    m_pageIndexBuffer.Shutdown();
    m_nodeUploadBuffer.Shutdown();
    m_pageUploadBuffer.Shutdown();
    m_pageIndexUploadBuffer.Shutdown();
    m_nodes.clear();
    m_pages.clear();
    m_pageIndex.clear();
    m_stats = {};
    m_pendingGpuCopies.clear();
    m_gpuUploadStage = GpuUploadStage::Idle;
    m_gpuUploadStageOffset = 0;
}

uint32_t FarVoxelOctree::BuildNode(const BuildBounds& bounds, uint32_t depth) {
    const uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.size());
    m_nodes.push_back(Node{});
    BuildNodeInto(nodeIndex, bounds, depth);
    return nodeIndex;
}

void FarVoxelOctree::BuildNodeInto(uint32_t nodeIndex, const BuildBounds& bounds, uint32_t depth) {
    if (depth > 0 && CellCanCollapseSolid(bounds)) {
        const float centerX = bounds.x + bounds.size * 0.5f;
        const float centerZ = bounds.z + bounds.size * 0.5f;
        const float height = TerrainHeight(centerX, centerZ);
        Node& leaf = m_nodes[nodeIndex];
        leaf.material = DominantMaterial(centerX, centerZ, height);
        leaf.flags = kLeafFlag | kInteriorLeafFlag;
        return;
    }

    if (depth >= m_config.maxDepth || bounds.size <= m_config.leafFloor) {
        const float centerX = bounds.x + bounds.size * 0.5f;
        const float centerZ = bounds.z + bounds.size * 0.5f;
        const float height = TerrainHeight(centerX, centerZ);
        Node& leaf = m_nodes[nodeIndex];
        leaf.material = DominantMaterial(centerX, centerZ, height);
        leaf.flags = kLeafFlag;
        return;
    }

    uint32_t childMask = 0;
    BuildBounds childBounds[8] = {};
    const float childSize = bounds.size * 0.5f;

    for (uint32_t child = 0; child < 8; ++child) {
        const BuildBounds boundsForChild{
            bounds.x + ((child & 1u) ? childSize : 0.0f),
            bounds.y + ((child & 2u) ? childSize : 0.0f),
            bounds.z + ((child & 4u) ? childSize : 0.0f),
            childSize
        };

        if (!CellMayContainTerrain(boundsForChild)) {
            continue;
        }

        childBounds[child] = boundsForChild;
        childMask |= (1u << child);
    }

    Node& node = m_nodes[nodeIndex];
    if (childMask == 0) {
        // If no child survives the terrain-overlap test, this node is an
        // empty/conservative skip region for visual far rendering. Treating it
        // as a solid leaf created large synthetic slabs in the far SVO pass.
        node.material = 0;
        node.flags = kLeafFlag | kInteriorLeafFlag;
    } else {
        const uint32_t childBase = static_cast<uint32_t>(m_nodes.size());
        node.childBase = childBase;
        node.childMask = childMask;
        node.flags = 0;

        const uint32_t childCount = CountBits(childMask);
        m_nodes.resize(m_nodes.size() + childCount);

        uint32_t compactIndex = 0;
        for (uint32_t child = 0; child < 8; ++child) {
            if ((childMask & (1u << child)) == 0) {
                continue;
            }

            BuildNodeInto(childBase + compactIndex, childBounds[child], depth + 1);
            ++compactIndex;
        }
    }
}

bool FarVoxelOctree::CellMayContainTerrain(const BuildBounds& bounds) const {
    const float minTerrainY = static_cast<float>(VENPOD::Simulation::TERRAIN_MIN_Y);
    const float maxTerrainY = static_cast<float>(VENPOD::Simulation::TERRAIN_MAX_Y);
    if (bounds.y > maxTerrainY || bounds.y + bounds.size < minTerrainY) {
        return false;
    }

    const float x0 = bounds.x;
    const float z0 = bounds.z;
    const float x1 = bounds.x + bounds.size;
    const float z1 = bounds.z + bounds.size;
    const float xc = bounds.x + bounds.size * 0.5f;
    const float zc = bounds.z + bounds.size * 0.5f;

    const float h0 = TerrainHeight(x0, z0);
    const float h1 = TerrainHeight(x1, z0);
    const float h2 = TerrainHeight(x0, z1);
    const float h3 = TerrainHeight(x1, z1);
    const float hc = TerrainHeight(xc, zc);
    const float hx0 = TerrainHeight(xc, z0);
    const float hx1 = TerrainHeight(xc, z1);
    const float hz0 = TerrainHeight(x0, zc);
    const float hz1 = TerrainHeight(x1, zc);
    const float q0 = TerrainHeight(bounds.x + bounds.size * 0.25f, bounds.z + bounds.size * 0.25f);
    const float q1 = TerrainHeight(bounds.x + bounds.size * 0.75f, bounds.z + bounds.size * 0.25f);
    const float q2 = TerrainHeight(bounds.x + bounds.size * 0.25f, bounds.z + bounds.size * 0.75f);
    const float q3 = TerrainHeight(bounds.x + bounds.size * 0.75f, bounds.z + bounds.size * 0.75f);
    float maxHeight = std::max({h0, h1, h2, h3, hc, hx0, hx1, hz0, hz1, q0, q1, q2, q3});
    if (bounds.size >= 48.0f) {
        for (uint32_t iz = 1; iz < 5; ++iz) {
            for (uint32_t ix = 1; ix < 5; ++ix) {
                const float sx = bounds.x + bounds.size * (static_cast<float>(ix) / 5.0f);
                const float sz = bounds.z + bounds.size * (static_cast<float>(iz) / 5.0f);
                maxHeight = std::max(maxHeight, TerrainHeight(sx, sz));
            }
        }
    }
    maxHeight += bounds.size * 0.55f;

    return bounds.y <= maxHeight && bounds.y + bounds.size >= minTerrainY;
}

bool FarVoxelOctree::CellCanCollapseSolid(const BuildBounds& bounds) const {
    const float x0 = bounds.x;
    const float z0 = bounds.z;
    const float x1 = bounds.x + bounds.size;
    const float z1 = bounds.z + bounds.size;
    const float xc = bounds.x + bounds.size * 0.5f;
    const float zc = bounds.z + bounds.size * 0.5f;
    const float xq0 = bounds.x + bounds.size * 0.25f;
    const float xq1 = bounds.x + bounds.size * 0.75f;
    const float zq0 = bounds.z + bounds.size * 0.25f;
    const float zq1 = bounds.z + bounds.size * 0.75f;

    float minHeight = std::min({
        TerrainHeight(x0, z0),
        TerrainHeight(x1, z0),
        TerrainHeight(x0, z1),
        TerrainHeight(x1, z1),
        TerrainHeight(xc, zc),
        TerrainHeight(xc, z0),
        TerrainHeight(xc, z1),
        TerrainHeight(x0, zc),
        TerrainHeight(x1, zc),
        TerrainHeight(xq0, zq0),
        TerrainHeight(xq1, zq0),
        TerrainHeight(xq0, zq1),
        TerrainHeight(xq1, zq1)
    });
    if (bounds.size >= 48.0f) {
        for (uint32_t iz = 1; iz < 5; ++iz) {
            for (uint32_t ix = 1; ix < 5; ++ix) {
                const float sx = bounds.x + bounds.size * (static_cast<float>(ix) / 5.0f);
                const float sz = bounds.z + bounds.size * (static_cast<float>(iz) / 5.0f);
                minHeight = std::min(minHeight, TerrainHeight(sx, sz));
            }
        }
    }

    // Collapse only cells safely below every sampled surface point. This keeps
    // detail near cliffs/ravines while avoiding a deep dense tree for solid
    // mountain interiors that are only used as far-field silhouette.
    return bounds.y + bounds.size < minHeight - bounds.size * 0.45f;
}

uint32_t FarVoxelOctree::DominantMaterial(float x, float z, float height) const {
    const float hx0 = TerrainHeight(x - 4.0f, z);
    const float hx1 = TerrainHeight(x + 4.0f, z);
    const float hz0 = TerrainHeight(x, z - 4.0f);
    const float hz1 = TerrainHeight(x, z + 4.0f);
    const float localRelief = std::max(
        std::max(std::abs(hx0 - height), std::abs(hx1 - height)),
        std::max(std::abs(hz0 - height), std::abs(hz1 - height)));

    if (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y)) {
        return kMaterialWater;
    }
    if (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 48.0f &&
        localRelief < 36.0f) {
        return kMaterialSand;
    }
    if (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 72.0f) {
        return (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 48.0f &&
                localRelief < 36.0f)
            ? kMaterialSand
            : kMaterialDirt;
    }
    if (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 128.0f) {
        return (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 86.0f &&
                localRelief < 58.0f)
            ? kMaterialSand
            : kMaterialDirt;
    }
    if (localRelief > 10.0f || height > 160.0f) {
        return kMaterialStone;
    }
    return kMaterialDirt;
}

float FarVoxelOctree::TerrainHeight(float x, float z) const {
    const float broad = FarTerrainValueNoise2D(x * 0.0045f, z * 0.0045f, m_config.seed + 11u);
    const float ridgeSource = FarTerrainValueNoise2D(
        x * 0.0100f + 41.0f,
        z * 0.0100f - 17.0f,
        m_config.seed + 23u);
    const float ridge = 1.0f - std::abs(ridgeSource);
    const float detail = FarTerrainValueNoise2D(
        x * 0.035f - 13.0f,
        z * 0.035f + 29.0f,
        m_config.seed + 37u);
    const float ridgeHeight = ridge * ridge;

    // VISUAL PASS iter1 (landforms): PARITY mirror of CPU HeightAt. broad 145 -> 92,
    // detail 8 -> 3. ridgeHeight kept at 150.
    float height = -64.0f;
    height += broad * 92.0f;
    height += ridgeHeight * 150.0f;
    height += detail * 3.0f;

    const float originDx = x - 192.0f;
    const float originDz = z - 224.0f;
    const float originDistance = std::sqrt(originDx * originDx + originDz * originDz);
    const float originComfort =
        1.0f - Smooth01(std::clamp((originDistance - 180.0f) / 520.0f, 0.0f, 1.0f));
    const float publicRegionHeight =
        -42.0f +
        broad * 54.0f +
        ridgeHeight * 48.0f +
        detail * 3.0f +
        (1.0f - Smooth01(originDistance / 360.0f)) * 72.0f;
    height += (1.0f - Smooth01(originDistance / 420.0f)) * 58.0f;
    height = height + (publicRegionHeight - height) * (originComfort * 0.94f);
    const float publicCapInfluence =
        1.0f - Smooth01(std::clamp((originDistance - 220.0f) / 420.0f, 0.0f, 1.0f));
    const float publicCap =
        58.0f + Smooth01(std::clamp(originDistance / 640.0f, 0.0f, 1.0f)) * 114.0f;
    const float cappedHeight = std::min(height, publicCap);
    height = height + (cappedHeight - height) * publicCapInfluence;

    const float submergedBlend =
        1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 28)) / 86.0f,
            0.0f,
            1.0f));
    if (submergedBlend > 0.0f) {
        const float submergedShelfHeight =
            static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y - 8) +
            broad * 38.0f +
            ridgeHeight * 22.0f +
            detail * 2.0f +
            (1.0f - Smooth01(originDistance / 520.0f)) * 18.0f;
        height = height + (submergedShelfHeight - height) * (submergedBlend * 0.55f);
    }

    const float playableBankBand =
        1.0f - Smooth01(std::clamp((originDistance - 260.0f) / 980.0f, 0.0f, 1.0f));
    const float lowlandUpper =
        1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 96)) / 120.0f,
            0.0f,
            1.0f));
    const float lowlandFloor =
        Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y - 40)) / 64.0f,
            0.0f,
            1.0f));
    const float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
    if (playableBankBlend > 0.0f) {
        const float playableShelfHeight =
            static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 18) +
            broad * 28.0f +
            ridgeHeight * 10.0f +
            detail * 1.5f +
            (1.0f - Smooth01(std::clamp(originDistance / 460.0f, 0.0f, 1.0f))) * 42.0f;
        height = height + (playableShelfHeight - height) * playableBankBlend;
    }
    const float publicBasinBand =
        Smooth01(std::clamp((originDistance - 360.0f) / 240.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 1700.0f) / 760.0f, 0.0f, 1.0f))) *
        Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y - 38)) / 56.0f,
            0.0f,
            1.0f)) *
        (1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 180)) / 140.0f,
            0.0f,
            1.0f)));
    const float publicBasinFloor =
        static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y - 12) +
        broad * 2.0f +
        detail * 0.35f;
    if (publicBasinBand > 0.0f) {
        const float basinHeight = std::min(height, publicBasinFloor);
        height = height + (basinHeight - height) * (publicBasinBand * 0.80f);
    }
    const float backdropNoise = FarTerrainValueNoise2D(
        x * 0.0018f + 19.0f,
        z * 0.0018f - 31.0f,
        m_config.seed + 211u);
    const float backdropRidgeSource = FarTerrainValueNoise2D(
        x * 0.0032f - 71.0f,
        z * 0.0032f + 43.0f,
        m_config.seed + 227u);
    const float backdropRidge = 1.0f - std::abs(backdropRidgeSource);
    const float backdropBreakup = FarTerrainValueNoise2D(
        x * 0.0075f + 203.0f,
        z * 0.0075f - 167.0f,
        m_config.seed + 271u);
    const float backdropNotch =
        Smooth01(std::clamp((backdropBreakup - 0.08f) / 0.58f, 0.0f, 1.0f));
    const float silhouetteRidge =
        std::clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f, 0.0f, 1.0f);
    const float backdropBand =
        Smooth01(std::clamp((originDistance - 1360.0f) / 700.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 5200.0f) / 1200.0f, 0.0f, 1.0f)));
    const float northBackdrop = Smooth01(std::clamp((z - 1180.0f) / 900.0f, 0.0f, 1.0f));
    const float sideBackdrop =
        Smooth01(std::clamp((std::abs(x - 192.0f) - 820.0f) / 980.0f, 0.0f, 1.0f));
    const float backdropFacing =
        std::clamp(northBackdrop + sideBackdrop * 0.58f, 0.0f, 1.0f);
    const float silhouetteContinuity =
        std::clamp(silhouetteRidge + backdropBand * backdropFacing * 0.32f, 0.0f, 1.0f);
    const float backdropInfluence =
        backdropBand *
        backdropFacing *
        Smooth01(silhouetteContinuity) *
        (0.46f + backdropNotch * 0.54f);
    const float backdropHeight =
        248.0f +
        backdropBand * 160.0f +
        silhouetteContinuity * 186.0f +
        backdropNoise * 26.0f;
    height = height + (std::max(height, backdropHeight) - height) * (backdropInfluence * 0.70f);

    const float westCorridor = Smooth01(std::clamp((192.0f - x - 520.0f) / 820.0f, 0.0f, 1.0f));
    const float eastCorridor = Smooth01(std::clamp((x - 192.0f - 520.0f) / 820.0f, 0.0f, 1.0f));
    const float southBlend = Smooth01(std::clamp((360.0f - z) / 1200.0f, 0.0f, 1.0f));
    const float westNorthBlend = Smooth01(std::clamp((z - 360.0f) / 920.0f, 0.0f, 1.0f));
    const float routeDistanceBand =
        Smooth01(std::clamp((originDistance - 780.0f) / 420.0f, 0.0f, 1.0f)) *
        (1.0f - Smooth01(std::clamp((originDistance - 4300.0f) / 1200.0f, 0.0f, 1.0f)));
    const float routeCorridor = routeDistanceBand * std::clamp(
        westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
        eastCorridor * southBlend,
        0.0f,
        1.0f);
    const float routeRidgeNoiseA = FarTerrainValueNoise2D(
        x * 0.0024f + 113.0f,
        z * 0.0024f - 89.0f,
        m_config.seed + 251u);
    const float routeRidgeNoiseB = FarTerrainValueNoise2D(
        x * 0.0068f - 37.0f,
        z * 0.0068f + 151.0f,
        m_config.seed + 263u);
    const float routeBreakup = FarTerrainValueNoise2D(
        x * 0.0110f - 211.0f,
        z * 0.0110f + 73.0f,
        m_config.seed + 281u);
    const float routeNotch =
        Smooth01(std::clamp((routeBreakup - 0.02f) / 0.60f, 0.0f, 1.0f));
    const float routeRidge =
        std::clamp(
            0.26f +
            (1.0f - std::abs(routeRidgeNoiseA)) * 0.58f +
            routeRidgeNoiseB * 0.16f,
            0.0f,
            1.0f);
    const float routeBackdropHeight =
        272.0f +
        routeDistanceBand * 104.0f +
        routeRidge * 218.0f;
    height = height + (std::max(height, routeBackdropHeight) - height) *
        (routeCorridor * routeRidge * routeNotch * 0.68f);

    // Spawn landmass: lift low/submerged near-origin terrain onto a gently
    // rolling land floor comfortably above sea level. TANDEM widen 9300 -> 35000
    // (continent fills the render range). Must match SparseTerrainGenerator::HeightAt
    // / TH_HeightAt (the geometry copies) - 35000.0f exactly.
    const float spawnLandBand =
        1.0f - Smooth01(std::clamp((originDistance - 200.0f) / 90000.0f, 0.0f, 1.0f));
    // VISUAL PASS iter1 (coast): PARITY mirror. +40 -> +56 base, noise softened.
    const float spawnLandFloor =
        static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 56.0f +
        broad * 18.0f +
        ridgeHeight * 40.0f +
        detail * 3.0f;
    height = height + (std::max(height, spawnLandFloor) - height) * spawnLandBand;

    // VISUAL PASS iter1 (small consistent block steps): PARITY mirror of CPU terrace
    // quantization (3-unit step on lowland/plains band, fading into hills).
    // VISUAL PASS iter2 (coherent shore): PARITY mirror — also fade the terrace OUT
    // near/below the waterline (off by SEA+8, full by SEA+40) for a smooth sloped
    // shore instead of a stepped staircase into the water plane.
    const float terraceStep = 3.0f;
    const float terraceUpperFade =
        1.0f - Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 64)) / 150.0f,
            0.0f,
            1.0f));
    const float terraceShoreFade =
        Smooth01(std::clamp(
            (height - static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y + 8)) / 32.0f,
            0.0f,
            1.0f));
    const float terraceBlend = terraceUpperFade * terraceShoreFade;
    if (terraceBlend > 0.0f) {
        const float terraced = std::floor(height / terraceStep) * terraceStep;
        height = height + (terraced - height) * terraceBlend;
    }

    return std::clamp(
        height,
        static_cast<float>(VENPOD::Simulation::TERRAIN_MIN_Y),
        static_cast<float>(VENPOD::Simulation::TERRAIN_MAX_Y));
}

bool FarVoxelOctree::BuildDagFromTree(
    const std::vector<Node>& treeNodes,
    const std::vector<Page>& treePages,
    std::vector<DagNodeGpu>& outNodes,
    std::vector<uint32_t>& outChildPointers,
    std::vector<DagPageGpu>& outPages)
{
    outNodes.clear();
    outChildPointers.clear();
    outPages.clear();
    const size_t nodeTotal = treeNodes.size();
    if (nodeTotal == 0) {
        return false;
    }
    outNodes.reserve(nodeTotal / 16 + 16);
    outChildPointers.reserve(nodeTotal / 8 + 16);

    std::vector<uint32_t> canon(nodeTotal, 0xFFFFFFFFu);  // tree index -> DAG node id
    std::unordered_map<std::string, uint32_t> uniqueMap;
    uniqueMap.reserve(nodeTotal / 2 + 16);
    std::string key;
    key.reserve(48);
    auto put = [&key](uint32_t value) {
        key.push_back(static_cast<char>(value & 0xFFu));
        key.push_back(static_cast<char>((value >> 8) & 0xFFu));
        key.push_back(static_cast<char>((value >> 16) & 0xFFu));
        key.push_back(static_cast<char>((value >> 24) & 0xFFu));
    };
    // Children sit at higher indices than their parent within each page block, so a
    // descending walk canonicalizes children before parents (the same ordering the
    // verified materialization measurement uses).
    for (size_t reverse = 0; reverse < nodeTotal; ++reverse) {
        const size_t index = nodeTotal - 1 - reverse;
        const Node& node = treeNodes[index];
        const bool leaf = (node.flags & kLeafFlag) != 0u ||
                          node.childMask == 0u ||
                          node.childBase == 0xFFFFFFFFu;
        key.clear();
        uint32_t childDagIds[8];
        uint32_t childDagCount = 0;
        if (leaf) {
            put(0xA11Eu);
            put(node.flags & (kLeafFlag | kInteriorLeafFlag));
            put(node.material);
        } else {
            put(0x1u);
            put(node.childMask);
            uint32_t compact = 0;
            for (uint32_t child = 0; child < 8; ++child) {
                if ((node.childMask & (1u << child)) == 0u) {
                    continue;
                }
                const uint32_t childIndex = node.childBase + compact;
                ++compact;
                const uint32_t childDagId =
                    (childIndex < nodeTotal) ? canon[childIndex] : 0xFFFFFFFFu;
                childDagIds[childDagCount++] = childDagId;
                put(childDagId);
            }
        }
        auto found = uniqueMap.find(key);
        if (found != uniqueMap.end()) {
            canon[index] = found->second;
            continue;
        }
        const uint32_t newId = static_cast<uint32_t>(outNodes.size());
        DagNodeGpu dag{};
        dag.material = node.material;
        dag.flags = node.flags;
        if (leaf) {
            dag.childMask = 0u;
            dag.childPtrBase = 0xFFFFFFFFu;
        } else {
            dag.childMask = node.childMask;
            dag.childPtrBase = static_cast<uint32_t>(outChildPointers.size());
            for (uint32_t k = 0; k < childDagCount; ++k) {
                outChildPointers.push_back(childDagIds[k]);
            }
        }
        outNodes.push_back(dag);
        uniqueMap.emplace(key, newId);
        canon[index] = newId;
    }

    outPages.reserve(treePages.size());
    for (const Page& page : treePages) {
        DagPageGpu dagPage{};
        dagPage.originX = page.originX;
        dagPage.originY = page.originY;
        dagPage.originZ = page.originZ;
        dagPage.rootNode =
            (page.rootNode < nodeTotal) ? canon[page.rootNode] : 0xFFFFFFFFu;
        outPages.push_back(dagPage);
    }
    return !outNodes.empty();
}

void FarVoxelOctree::EnsureDagResident(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    DescriptorHeapManager& heapManager)
{
    if (m_dagReady || m_dagBuildAttempted || !device || !cmdList) {
        return;
    }
    const char* enable = std::getenv("VENPOD_FARVOXEL_DAG");
    if (!(enable && enable[0] == '1')) {
        return;  // P2 feature flag off -> DAG never built (zero cost, engine unchanged)
    }
    if (m_nodes.empty() || m_pages.empty()) {
        return;  // tree not resident yet; retry on a later frame
    }
    m_dagBuildAttempted = true;

    const auto buildStart = std::chrono::steady_clock::now();
    std::vector<DagNodeGpu> dagNodes;
    std::vector<uint32_t> childPointers;
    std::vector<DagPageGpu> dagPages;
    if (!BuildDagFromTree(m_nodes, m_pages, dagNodes, childPointers, dagPages) ||
        dagNodes.empty() || childPointers.empty() || dagPages.empty()) {
        spdlog::warn("FarVoxelOctree::EnsureDagResident - DAG build produced no data");
        return;
    }

    const uint64_t nodeBytes = static_cast<uint64_t>(dagNodes.size()) * sizeof(DagNodeGpu);
    const uint64_t ptrBytes = static_cast<uint64_t>(childPointers.size()) * sizeof(uint32_t);
    const uint64_t pageBytes = static_cast<uint64_t>(dagPages.size()) * sizeof(DagPageGpu);

    auto upload = [&](GPUBuffer& buffer, const void* data, uint64_t bytes, uint32_t stride,
                      const char* name) -> bool {
        auto result = buffer.InitializeWithData(
            device, cmdList, data, bytes,
            BufferUsage::Default | BufferUsage::StructuredBuffer, stride, name);
        if (!result) {
            spdlog::warn("FarVoxelOctree::EnsureDagResident - {} upload failed: {}",
                         name, result.error());
            return false;
        }
        auto srv = buffer.CreateSRV(device, heapManager);
        if (!srv) {
            spdlog::warn("FarVoxelOctree::EnsureDagResident - {} SRV failed: {}",
                         name, srv.error());
            return false;
        }
        buffer.TransitionTo(
            cmdList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    };

    if (!upload(m_dagNodeBuffer, dagNodes.data(), nodeBytes, sizeof(DagNodeGpu),
                "FarVoxelDag_Nodes") ||
        !upload(m_dagChildPtrBuffer, childPointers.data(), ptrBytes, sizeof(uint32_t),
                "FarVoxelDag_ChildPtrs") ||
        !upload(m_dagPageBuffer, dagPages.data(), pageBytes, sizeof(DagPageGpu),
                "FarVoxelDag_Pages")) {
        return;
    }

    m_dagNodeCount = dagNodes.size();
    m_dagChildPtrCount = childPointers.size();
    m_dagPageCount = dagPages.size();
    m_dagReady = true;

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - buildStart).count();
    const double mib =
        static_cast<double>(nodeBytes + ptrBytes + pageBytes) / (1024.0 * 1024.0);
    spdlog::info(
        "[DAG-RESIDENT] uploaded DAG: nodes={} ptrs={} pages={} ({:.1f} MiB) in {:.1f} ms; "
        "SRVs valid={}",
        m_dagNodeCount, m_dagChildPtrCount, m_dagPageCount, mib, ms,
        (GetDagNodeSRV().IsValid() && GetDagChildPtrSRV().IsValid() &&
         GetDagPageSRV().IsValid())
            ? 1
            : 0);
}

} // namespace VENPOD::Graphics
