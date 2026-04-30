#pragma once

#include "RHI/GPUBuffer.h"
#include "RHI/DescriptorHeap.h"
#include "../Utils/Result.h"
#include <cstdint>
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
    uint32_t maxDepth = 0;
    float pageSize = 0.0f;
    float coveredWorldSize = 0.0f;
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

    void Shutdown();

    bool IsValid() const { return m_nodeBuffer.GetResource() && m_pageBuffer.GetResource(); }

    const DescriptorHandle& GetNodeSRV() const { return m_nodeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetPageSRV() const { return m_pageBuffer.GetShaderVisibleSRV(); }
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

    FarVoxelOctreeConfig m_config;
    FarVoxelOctreeStats m_stats;
    std::vector<Node> m_nodes;
    std::vector<Page> m_pages;
    GPUBuffer m_nodeBuffer;
    GPUBuffer m_pageBuffer;
};

} // namespace VENPOD::Graphics
