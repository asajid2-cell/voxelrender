#pragma once

#include "RHI/GPUBuffer.h"
#include "RHI/DescriptorHeap.h"
#include "../Utils/Result.h"
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace VENPOD::Graphics {

struct FarVoxelOctreeConfig {
    int32_t pageRadius = 4;
    float pageSize = 1024.0f;
    float rootMinY = -384.0f;
    uint32_t maxDepth = 6;
    uint32_t seed = 12345;
};

struct FarVoxelOctreeStats {
    uint32_t pageCount = 0;
    uint32_t nodeCount = 0;
    uint32_t pageIndexCount = 0;
    int32_t pageRadius = 0;
    uint32_t maxDepth = 0;
    float pageSize = 0.0f;
    float rootMinY = 0.0f;
    float coveredWorldSize = 0.0f;
    double cpuBuildMs = 0.0;
    double gpuUploadMs = 0.0;
    bool loadedFromCache = false;
};

class FarVoxelOctree {
public:
    FarVoxelOctree() = default;
    ~FarVoxelOctree() = default;

    FarVoxelOctree(const FarVoxelOctree&) = delete;
    FarVoxelOctree& operator=(const FarVoxelOctree&) = delete;

    Result<void> Initialize(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        const FarVoxelOctreeConfig& config = {});
    void BeginAsyncLoad(const FarVoxelOctreeConfig& config = {});
    bool IsAsyncPending() const { return m_asyncPending; }
    bool TryFinalizeAsyncUpload(ID3D12Device* device, DescriptorHeapManager& heapManager);

    void Shutdown();

    bool IsValid() const { return m_nodeBuffer.GetResource() && m_pageBuffer.GetResource(); }

    const DescriptorHandle& GetNodeSRV() const { return m_nodeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetPageSRV() const { return m_pageBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetPageIndexSRV() const { return m_pageIndexBuffer.GetShaderVisibleSRV(); }
    const FarVoxelOctreeStats& GetStats() const { return m_stats; }

private:
    struct Node {
        uint32_t childBase = 0xFFFFFFFFu;
        uint32_t childMask = 0;
        uint32_t material = 0;
        uint32_t flags = 0;
    };

    struct Page {
        int32_t originX = 0;
        int32_t originY = 0;
        int32_t originZ = 0;
        uint32_t rootNode = 0;
    };

    struct BuildBounds {
        float x;
        float y;
        float z;
        float size;
    };

    uint32_t BuildNode(const BuildBounds& bounds, uint32_t depth);
    void BuildNodeInto(uint32_t nodeIndex, const BuildBounds& bounds, uint32_t depth);
    bool CellMayContainTerrain(const BuildBounds& bounds) const;
    bool CellCanCollapseSolid(const BuildBounds& bounds) const;
    uint32_t DominantMaterial(float x, float z, float height) const;
    float TerrainHeight(float x, float z) const;
    Result<void> UploadToGpu(ID3D12Device* device, DescriptorHeapManager& heapManager);

    struct BuildResult {
        bool success = false;
        std::string error;
        FarVoxelOctreeConfig config;
        FarVoxelOctreeStats stats;
        std::vector<Node> nodes;
        std::vector<Page> pages;
        std::vector<uint32_t> pageIndex;
    };

    static BuildResult BuildCpuData(const FarVoxelOctreeConfig& config);

    FarVoxelOctreeConfig m_config;
    FarVoxelOctreeStats m_stats;
    std::vector<Node> m_nodes;
    std::vector<Page> m_pages;
    std::vector<uint32_t> m_pageIndex;
    GPUBuffer m_nodeBuffer;
    GPUBuffer m_pageBuffer;
    GPUBuffer m_pageIndexBuffer;
    std::future<BuildResult> m_asyncBuild;
    bool m_asyncPending = false;
};

} // namespace VENPOD::Graphics
