#pragma once

#include "RHI/GPUBuffer.h"
#include "RHI/DescriptorHeap.h"
#include "../Utils/Result.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <future>
#include <limits>
#include <string>
#include <vector>

namespace VENPOD::Graphics {

struct FarVoxelOctreeConfig {
    int32_t pageRadius = 12;
    float pageSize = 1024.0f;
    float rootMinY = -384.0f;
    uint32_t maxDepth = 6;
    uint32_t seed = 12345;
    // Finest leaf cell size (world units). Lowering this past the default 16 (and
    // raising maxDepth to match) gives finer far/DAG voxels. The DAG's ~39x subtree
    // de-dup keeps the compressed size affordable as this shrinks.
    float leafFloor = 16.0f;
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
    uint64_t gpuUploadBytesTotal = 0;
    uint64_t gpuUploadBytesUploaded = 0;
    uint64_t gpuUploadStageBytesTotal = 0;
    uint64_t gpuUploadStageBytesUploaded = 0;
    uint32_t gpuUploadStage = 0;
    bool loadedFromCache = false;
};

struct FarVoxelOctreeResidencyMetadata {
    float uploadCoverageRatio = 0.0f;
    float pageCoverageRatio = 0.0f;
    bool ready = false;
};

inline bool TryFloorFarVoxelDoubleToInt32(double value) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double floored = std::floor(value);
    return floored >= static_cast<double>(std::numeric_limits<int32_t>::min()) &&
           floored <= static_cast<double>(std::numeric_limits<int32_t>::max());
}

inline bool ValidateFarVoxelOctreeConfigForBuild(
    const FarVoxelOctreeConfig& config,
    std::string* outError = nullptr)
{
    auto fail = [outError](const char* message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };

    constexpr uint32_t kMaxBuildDepth = 12;
    if (!std::isfinite(config.pageSize) || config.pageSize <= 0.0f) {
        return fail("FarVoxelOctree pageSize must be finite and positive");
    }
    if (!std::isfinite(config.rootMinY)) {
        return fail("FarVoxelOctree rootMinY must be finite");
    }
    if (config.maxDepth > kMaxBuildDepth) {
        return fail("FarVoxelOctree maxDepth exceeds runtime limit");
    }
    if (!TryFloorFarVoxelDoubleToInt32(static_cast<double>(config.rootMinY))) {
        return fail("FarVoxelOctree rootMinY is outside int32 world range");
    }

    const int32_t radius = std::max(1, config.pageRadius);
    const double pageSize = static_cast<double>(config.pageSize);
    const double radiusDouble = static_cast<double>(radius);
    if (!TryFloorFarVoxelDoubleToInt32(-radiusDouble * pageSize) ||
        !TryFloorFarVoxelDoubleToInt32(radiusDouble * pageSize)) {
        return fail("FarVoxelOctree page origins exceed int32 world range");
    }
    return true;
}

inline FarVoxelOctreeResidencyMetadata BuildFarVoxelOctreeResidencyMetadata(
    const FarVoxelOctreeStats& stats,
    bool buffersValid) {
    FarVoxelOctreeResidencyMetadata metadata;
    metadata.uploadCoverageRatio = stats.gpuUploadBytesTotal > 0
        ? std::clamp(
            static_cast<float>(
                static_cast<double>(stats.gpuUploadBytesUploaded) /
                static_cast<double>(stats.gpuUploadBytesTotal)),
            0.0f,
            1.0f)
        : 0.0f;
    metadata.pageCoverageRatio = stats.pageIndexCount > 0
        ? std::clamp(
            static_cast<float>(
                static_cast<double>(stats.pageCount) /
                static_cast<double>(stats.pageIndexCount)),
            0.0f,
            1.0f)
        : 0.0f;
    metadata.ready =
        buffersValid &&
        metadata.uploadCoverageRatio >= 0.999f &&
        metadata.pageCoverageRatio > 0.0f &&
        stats.nodeCount > 0 &&
        stats.pageCount > 0;
    return metadata;
}

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
    static bool HasCompatibleCache(const FarVoxelOctreeConfig& config);
    bool IsAsyncPending() const { return m_asyncPending; }
    bool IsGpuUploadPending() const;
    bool HasPendingGpuUploadCopies() const;
    bool EmitPendingGpuUploadCopies(ID3D12GraphicsCommandList* commandList);
    const char* GetGpuUploadStageName() const;
    bool TryFinalizeAsyncUpload(
        ID3D12Device* device,
        DescriptorHeapManager& heapManager,
        uint64_t maxUploadBytes = UINT64_MAX,
        uint32_t maxCpuWaitMs = 0);

    void Shutdown();

    bool IsValid() const { return m_nodeBuffer.GetResource() && m_pageBuffer.GetResource(); }

    const DescriptorHandle& GetNodeSRV() const { return m_nodeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetPageSRV() const { return m_pageBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetPageIndexSRV() const { return m_pageIndexBuffer.GetShaderVisibleSRV(); }
    const FarVoxelOctreeStats& GetStats() const { return m_stats; }

    // --- Editable SVDAG (P2) -------------------------------------------------
    // Pointer-indirection DAG de-duplicated from the tree (m_nodes). 16B fixed node
    // record (logical-id addressable) + a flat child-pointer array, so two parents can
    // share a subtree. Consumed by the separate PS_DagRaymarch pass (NOT PS_Raymarch).
    struct DagNodeGpu {
        uint32_t childPtrBase;  // index into the child-pointer array (0xFFFFFFFF for leaves)
        uint32_t childMask;     // 8-bit octant occupancy (0 for leaves)
        uint32_t material;
        uint32_t flags;         // kLeafFlag(1) | kInteriorLeafFlag(2)
    };
    struct DagPageGpu {
        int32_t originX;
        int32_t originY;
        int32_t originZ;
        uint32_t rootNode;      // index into the DAG node pool
    };

    // Build + one-shot upload the DAG from the resident tree (idempotent; cheap ~17 MiB).
    // No-op unless the VENPOD_FARVOXEL_DAG flag is set. Records the copy on cmdList; safe to
    // call every frame once the tree is valid (builds at most once).
    void EnsureDagResident(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        DescriptorHeapManager& heapManager);
    bool IsDagReady() const { return m_dagReady; }
    uint32_t GetDagNodeCount() const { return static_cast<uint32_t>(m_dagNodeCount); }
    uint32_t GetDagPageCount() const { return static_cast<uint32_t>(m_dagPageCount); }
    const DescriptorHandle& GetDagNodeSRV() const { return m_dagNodeBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetDagChildPtrSRV() const { return m_dagChildPtrBuffer.GetShaderVisibleSRV(); }
    const DescriptorHandle& GetDagPageSRV() const { return m_dagPageBuffer.GetShaderVisibleSRV(); }

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
    bool PumpGpuUpload(ID3D12Device* device, DescriptorHeapManager& heapManager, uint64_t maxUploadBytes);

    // Bottom-up de-dup of the tree (treeNodes/pages) into a pointer-indirection DAG.
    // Returns false on structural failure. Fills outNodes/outChildPointers/outPages.
    static bool BuildDagFromTree(
        const std::vector<Node>& treeNodes,
        const std::vector<Page>& treePages,
        std::vector<DagNodeGpu>& outNodes,
        std::vector<uint32_t>& outChildPointers,
        std::vector<DagPageGpu>& outPages);

    FarVoxelOctreeConfig m_config;
    FarVoxelOctreeStats m_stats;
    std::vector<Node> m_nodes;
    std::vector<Page> m_pages;
    std::vector<uint32_t> m_pageIndex;
    GPUBuffer m_nodeBuffer;
    GPUBuffer m_pageBuffer;
    GPUBuffer m_pageIndexBuffer;
    UploadBuffer m_nodeUploadBuffer;
    UploadBuffer m_pageUploadBuffer;
    UploadBuffer m_pageIndexUploadBuffer;
    std::future<BuildResult> m_asyncBuild;
    enum class GpuUploadStage {
        Idle,
        Nodes,
        Pages,
        PageIndex,
        Complete
    };
    struct PendingGpuCopy {
        GpuUploadStage stage = GpuUploadStage::Idle;
        uint64_t offset = 0;
        uint64_t bytes = 0;
    };
    std::vector<PendingGpuCopy> m_pendingGpuCopies;
    GpuUploadStage m_gpuUploadStage = GpuUploadStage::Idle;
    uint64_t m_gpuUploadStageOffset = 0;
    bool m_asyncPending = false;

    // --- Editable SVDAG (P2) GPU residency ----------------------------------
    GPUBuffer m_dagNodeBuffer;
    GPUBuffer m_dagChildPtrBuffer;
    GPUBuffer m_dagPageBuffer;
    UploadBuffer m_dagNodeUploadBuffer;
    UploadBuffer m_dagChildPtrUploadBuffer;
    UploadBuffer m_dagPageUploadBuffer;
    size_t m_dagNodeCount = 0;
    size_t m_dagChildPtrCount = 0;
    size_t m_dagPageCount = 0;
    bool m_dagBuildAttempted = false;
    bool m_dagReady = false;
};

} // namespace VENPOD::Graphics
