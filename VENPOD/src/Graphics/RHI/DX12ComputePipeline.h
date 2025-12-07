#pragma once

// =============================================================================
// VENPOD DX12 Compute Pipeline - Compute PSO wrapper
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include "ShaderCompiler.h"
#include "DX12GraphicsPipeline.h"  // For RootParamType
#include "../../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Root parameter for compute pipeline (uses RootParamType from DX12GraphicsPipeline.h)
struct ComputeRootParam {
    RootParamType type = RootParamType::ConstantBuffer;
    uint32_t shaderRegister = 0;
    uint32_t registerSpace = 0;
    uint32_t numDescriptors = 1;  // For descriptor tables or constants count
    D3D12_DESCRIPTOR_RANGE_TYPE rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;  // For descriptor tables
};

// Compute pipeline description
struct ComputePipelineDesc {
    CompiledShader computeShader;
    std::vector<ComputeRootParam> rootParams;
    std::string debugName;
};

class DX12ComputePipeline {
public:
    DX12ComputePipeline() = default;
    ~DX12ComputePipeline() = default;

    // Non-copyable
    DX12ComputePipeline(const DX12ComputePipeline&) = delete;
    DX12ComputePipeline& operator=(const DX12ComputePipeline&) = delete;

    Result<void> Initialize(ID3D12Device* device, const ComputePipelineDesc& desc);
    void Shutdown();

    // Bind pipeline for dispatch
    void Bind(ID3D12GraphicsCommandList* cmdList);

    // Set root parameters
    void SetRootConstantBufferView(ID3D12GraphicsCommandList* cmdList, uint32_t rootIndex, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress);
    void SetRootDescriptorTable(ID3D12GraphicsCommandList* cmdList, uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void SetRoot32BitConstants(ID3D12GraphicsCommandList* cmdList, uint32_t rootIndex, uint32_t num32BitValues, const void* data, uint32_t destOffset = 0);

    // Dispatch helpers
    void Dispatch(ID3D12GraphicsCommandList* cmdList, uint32_t threadGroupCountX, uint32_t threadGroupCountY = 1, uint32_t threadGroupCountZ = 1);

    // Accessors
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
    ID3D12PipelineState* GetPipelineState() const { return m_pipelineState.Get(); }
    bool IsValid() const { return m_pipelineState != nullptr; }

private:
    Result<void> CreateRootSignature(ID3D12Device* device, const ComputePipelineDesc& desc);
    Result<void> CreatePipelineState(ID3D12Device* device, const ComputePipelineDesc& desc);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    std::string m_debugName;
};

} // namespace VENPOD::Graphics
