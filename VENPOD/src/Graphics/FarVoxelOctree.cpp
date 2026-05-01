#include "FarVoxelOctree.h"
#include "../Simulation/TerrainConstants.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

namespace {
constexpr uint32_t kMaterialSand = 1;
constexpr uint32_t kMaterialStone = 3;
constexpr uint32_t kMaterialDirt = 4;
constexpr uint32_t kMaterialConcrete = 14;
constexpr uint32_t kLeafFlag = 1;

float Smooth01(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float Ridged(float value, float power) {
    return std::pow(std::clamp(1.0f - std::abs(value), 0.0f, 1.0f), power);
}

uint32_t CountBits(uint32_t value) {
    uint32_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
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

bool FarVoxelOctree::TryFinalizeAsyncUpload(ID3D12Device* device, DescriptorHeapManager& heapManager) {
    if (!m_asyncPending || !m_asyncBuild.valid()) {
        return IsValid();
    }

    if (m_asyncBuild.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
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

    auto uploadResult = UploadToGpu(device, heapManager);
    if (!uploadResult) {
        spdlog::warn("Far sparse voxel octree async upload failed: {}", uploadResult.error());
        Shutdown();
        return false;
    }
    return true;
}

FarVoxelOctree::BuildResult FarVoxelOctree::BuildCpuData(const FarVoxelOctreeConfig& config) {
    const auto buildStart = std::chrono::steady_clock::now();
    BuildResult result;
    result.config = config;

    FarVoxelOctree builder;
    builder.m_config = config;

    const int32_t radius = std::max(1, builder.m_config.pageRadius);
    const int32_t pageCountPerAxis = radius * 2 + 1;
    constexpr uint32_t kMissingPage = 0xFFFFFFFFu;

    struct CacheHeader {
        uint32_t magic = 0x56534631u; // "VSF1"
        uint32_t version = 2;
        int32_t pageRadius = 0;
        uint32_t maxDepth = 0;
        uint32_t seed = 0;
        float pageSize = 0.0f;
        float rootMinY = 0.0f;
        uint64_t nodeCount = 0;
        uint64_t pageCount = 0;
        uint64_t pageIndexCount = 0;
    };

    const auto cachePath = std::filesystem::current_path() /
        ("venpod_far_svo_cache_r" + std::to_string(radius) +
         "_d" + std::to_string(builder.m_config.maxDepth) +
         "_s" + std::to_string(builder.m_config.seed) + ".bin");

    bool loadedFromCache = false;
    {
        std::ifstream in(cachePath, std::ios::binary);
        CacheHeader header{};
        if (in.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == 0x56534631u &&
            header.version == 2 &&
            header.pageRadius == radius &&
            header.maxDepth == builder.m_config.maxDepth &&
            header.seed == builder.m_config.seed &&
            header.pageSize == builder.m_config.pageSize &&
            header.rootMinY == builder.m_config.rootMinY &&
            header.nodeCount > 0 &&
            header.pageCount > 0 &&
            header.pageIndexCount == static_cast<uint64_t>(pageCountPerAxis * pageCountPerAxis)) {

            builder.m_nodes.resize(static_cast<size_t>(header.nodeCount));
            builder.m_pages.resize(static_cast<size_t>(header.pageCount));
            builder.m_pageIndex.resize(static_cast<size_t>(header.pageIndexCount));

            if (in.read(reinterpret_cast<char*>(builder.m_nodes.data()), static_cast<std::streamsize>(builder.m_nodes.size() * sizeof(Node))) &&
                in.read(reinterpret_cast<char*>(builder.m_pages.data()), static_cast<std::streamsize>(builder.m_pages.size() * sizeof(Page))) &&
                in.read(reinterpret_cast<char*>(builder.m_pageIndex.data()), static_cast<std::streamsize>(builder.m_pageIndex.size() * sizeof(uint32_t)))) {
                loadedFromCache = true;
            } else {
                builder.m_nodes.clear();
                builder.m_pages.clear();
                builder.m_pageIndex.clear();
            }
        }
    }

    if (!loadedFromCache) {
        builder.m_nodes.reserve(static_cast<size_t>(pageCountPerAxis * pageCountPerAxis * 64));
        builder.m_pages.reserve(static_cast<size_t>(pageCountPerAxis * pageCountPerAxis));
        builder.m_pageIndex.assign(static_cast<size_t>(pageCountPerAxis * pageCountPerAxis), kMissingPage);

        for (int32_t pz = -radius; pz <= radius; ++pz) {
            for (int32_t px = -radius; px <= radius; ++px) {
                const float originX = static_cast<float>(px) * builder.m_config.pageSize;
                const float originZ = static_cast<float>(pz) * builder.m_config.pageSize;
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
                page.originX = static_cast<int32_t>(std::floor(originX));
                page.originY = static_cast<int32_t>(std::floor(builder.m_config.rootMinY));
                page.originZ = static_cast<int32_t>(std::floor(originZ));
                page.rootNode = builder.BuildNode(rootBounds, 0);
                const uint32_t pageIndex = static_cast<uint32_t>(builder.m_pages.size());
                const size_t denseIndex = static_cast<size_t>(pz + radius) *
                                          static_cast<size_t>(pageCountPerAxis) +
                                          static_cast<size_t>(px + radius);
                builder.m_pageIndex[denseIndex] = pageIndex;
                builder.m_pages.push_back(page);
            }
        }

        CacheHeader header{};
        header.pageRadius = radius;
        header.maxDepth = builder.m_config.maxDepth;
        header.seed = builder.m_config.seed;
        header.pageSize = builder.m_config.pageSize;
        header.rootMinY = builder.m_config.rootMinY;
        header.nodeCount = static_cast<uint64_t>(builder.m_nodes.size());
        header.pageCount = static_cast<uint64_t>(builder.m_pages.size());
        header.pageIndexCount = static_cast<uint64_t>(builder.m_pageIndex.size());

        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out.write(reinterpret_cast<const char*>(&header), sizeof(header)) &&
            out.write(reinterpret_cast<const char*>(builder.m_nodes.data()), static_cast<std::streamsize>(builder.m_nodes.size() * sizeof(Node))) &&
            out.write(reinterpret_cast<const char*>(builder.m_pages.data()), static_cast<std::streamsize>(builder.m_pages.size() * sizeof(Page))) &&
            out.write(reinterpret_cast<const char*>(builder.m_pageIndex.data()), static_cast<std::streamsize>(builder.m_pageIndex.size() * sizeof(uint32_t)))) {
            spdlog::info("Saved far voxel octree cache: {}", cachePath.string());
        }
    }

    if (builder.m_nodes.empty() || builder.m_pages.empty()) {
        result.error = "FarVoxelOctree generated no nodes/pages";
        return result;
    }

    result.stats.pageCount = static_cast<uint32_t>(builder.m_pages.size());
    result.stats.nodeCount = static_cast<uint32_t>(builder.m_nodes.size());
    result.stats.pageIndexCount = static_cast<uint32_t>(builder.m_pageIndex.size());
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
    const auto uploadStart = std::chrono::steady_clock::now();
    if (!device) {
        return Error("FarVoxelOctree::UploadToGpu - device is null");
    }
    if (m_nodes.empty() || m_pages.empty() || m_pageIndex.empty()) {
        return Error("FarVoxelOctree::UploadToGpu - CPU data is empty");
    }

    auto nodeResult = m_nodeBuffer.Initialize(
        device,
        static_cast<uint64_t>(m_nodes.size() * sizeof(Node)),
        BufferUsage::Upload | BufferUsage::StructuredBuffer,
        sizeof(Node),
        "FarVoxelOctree_Nodes");
    if (!nodeResult) {
        return Error("Failed to create far octree node buffer: {}", nodeResult.error());
    }
    m_nodeBuffer.Upload(m_nodes.data(), m_nodes.size() * sizeof(Node));
    nodeResult = m_nodeBuffer.CreateSRV(device, heapManager);
    if (!nodeResult) {
        return Error("Failed to create far octree node SRV: {}", nodeResult.error());
    }

    auto pageResult = m_pageBuffer.Initialize(
        device,
        static_cast<uint64_t>(m_pages.size() * sizeof(Page)),
        BufferUsage::Upload | BufferUsage::StructuredBuffer,
        sizeof(Page),
        "FarVoxelOctree_Pages");
    if (!pageResult) {
        return Error("Failed to create far octree page buffer: {}", pageResult.error());
    }
    m_pageBuffer.Upload(m_pages.data(), m_pages.size() * sizeof(Page));
    pageResult = m_pageBuffer.CreateSRV(device, heapManager);
    if (!pageResult) {
        return Error("Failed to create far octree page SRV: {}", pageResult.error());
    }

    auto indexResult = m_pageIndexBuffer.Initialize(
        device,
        static_cast<uint64_t>(m_pageIndex.size() * sizeof(uint32_t)),
        BufferUsage::Upload | BufferUsage::StructuredBuffer,
        sizeof(uint32_t),
        "FarVoxelOctree_PageIndex");
    if (!indexResult) {
        return Error("Failed to create far octree page index buffer: {}", indexResult.error());
    }
    m_pageIndexBuffer.Upload(m_pageIndex.data(), m_pageIndex.size() * sizeof(uint32_t));
    indexResult = m_pageIndexBuffer.CreateSRV(device, heapManager);
    if (!indexResult) {
        return Error("Failed to create far octree page index SRV: {}", indexResult.error());
    }

    m_stats.gpuUploadMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - uploadStart).count();
    spdlog::info("Far voxel octree initialized: {} pages, {} nodes, {} page-index cells, pageSize={}, coverage={} world units, source={}, {:.1f} ms",
        m_stats.pageCount,
        m_stats.nodeCount,
        m_stats.pageIndexCount,
        m_stats.pageSize,
        m_stats.coveredWorldSize,
        m_stats.loadedFromCache ? "cache" : "build",
        m_stats.cpuBuildMs + m_stats.gpuUploadMs);
    return {};
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
    m_nodes.clear();
    m_pages.clear();
    m_pageIndex.clear();
    m_stats = {};
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
        leaf.flags = kLeafFlag;
        return;
    }

    if (depth >= m_config.maxDepth || bounds.size <= 16.0f) {
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
        const float centerX = bounds.x + bounds.size * 0.5f;
        const float centerZ = bounds.z + bounds.size * 0.5f;
        const float height = TerrainHeight(centerX, centerZ);
        node.material = DominantMaterial(centerX, centerZ, height);
        node.flags = kLeafFlag;
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
    const float maxHeight = std::max({h0, h1, h2, h3, hc}) + bounds.size * 0.35f;

    return bounds.y <= maxHeight && bounds.y + bounds.size >= minTerrainY;
}

bool FarVoxelOctree::CellCanCollapseSolid(const BuildBounds& bounds) const {
    const float x0 = bounds.x;
    const float z0 = bounds.z;
    const float x1 = bounds.x + bounds.size;
    const float z1 = bounds.z + bounds.size;
    const float xc = bounds.x + bounds.size * 0.5f;
    const float zc = bounds.z + bounds.size * 0.5f;

    const float minHeight = std::min({
        TerrainHeight(x0, z0),
        TerrainHeight(x1, z0),
        TerrainHeight(x0, z1),
        TerrainHeight(x1, z1),
        TerrainHeight(xc, zc)
    });

    // Collapse only cells safely below every sampled surface point. This keeps
    // detail near cliffs/ravines while avoiding a deep dense tree for solid
    // mountain interiors that are only used as far-field silhouette.
    return bounds.y + bounds.size < minHeight - bounds.size * 0.20f;
}

uint32_t FarVoxelOctree::DominantMaterial(float x, float z, float height) const {
    const float materialNoise =
        std::sin(x * 0.013f + z * 0.017f + std::sin(x * 0.004f - z * 0.011f)) * 0.5f + 0.5f;

    if (height < static_cast<float>(VENPOD::Simulation::SEA_LEVEL_Y) + 4.0f) {
        return kMaterialSand;
    }
    if (height > 430.0f) {
        return kMaterialStone;
    }
    if (height > 220.0f) {
        return materialNoise > 0.45f ? kMaterialStone : kMaterialConcrete;
    }
    if (materialNoise > 0.84f) {
        return kMaterialConcrete;
    }
    return kMaterialDirt;
}

float FarVoxelOctree::TerrainHeight(float x, float z) const {
    const float n0 = std::sin(x * 0.00173f + z * 0.00091f + 2.1f);
    const float n1 = std::sin(x * -0.00077f + z * 0.00148f + 5.7f);
    const float n2 = std::sin(x * 0.00320f + z * -0.00260f + 1.3f);
    const float n3 = std::sin(x * -0.00510f + z * 0.00430f + 8.4f);

    const float continent = n0 * 0.58f + n1 * 0.42f;
    const float mountainMask = Smooth01((continent + 0.20f) * 0.95f);
    const float ridgeA = Ridged(n2, 1.35f);
    const float ridgeB = Ridged(n3, 1.90f);
    const float broadValley = Ridged(std::sin(x * 0.00092f + z * 0.00111f + 0.4f), 1.2f);
    const float spireMask =
        std::pow(Ridged(std::sin(x * 0.0078f + z * -0.0062f + n1), 2.0f), 3.5f) *
        (0.25f + mountainMask * 0.85f);
    const float ravineMask =
        1.0f - Smooth01((std::abs(std::sin(x * 0.00135f + z * -0.00105f + 2.6f)) - 0.02f) / 0.10f);

    const float dx = x - 96.0f;
    const float dz = z - 96.0f;
    const float originUplift = (1.0f - Smooth01(std::sqrt(dx * dx + dz * dz) / 420.0f)) * 170.0f;

    float height = -85.0f;
    height += continent * 210.0f;
    height += ridgeA * (125.0f + mountainMask * 170.0f);
    height += ridgeB * mountainMask * 95.0f;
    height += spireMask * 360.0f;
    height += originUplift;
    height -= broadValley * (90.0f - mountainMask * 30.0f);
    height -= ravineMask * 230.0f;

    const float terraceStep = 10.0f + (22.0f - 10.0f) * mountainMask;
    const float terraced = std::floor(height / terraceStep) * terraceStep;
    height = height * (1.0f - (0.26f + mountainMask * 0.20f)) +
             terraced * (0.26f + mountainMask * 0.20f);

    return std::clamp(
        height,
        static_cast<float>(VENPOD::Simulation::TERRAIN_MIN_Y),
        static_cast<float>(VENPOD::Simulation::TERRAIN_MAX_Y));
}

} // namespace VENPOD::Graphics
