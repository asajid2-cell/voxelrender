#pragma once

// =============================================================================
// VENPOD DX12 Graphics Pipeline - PSO and Root Signature management
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include "ShaderCompiler.h"
#include "../../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Root signature parameter types
enum class RootParamType {
    ConstantBuffer,      // CBV
    ShaderResource,      // SRV
    UnorderedAccess,     // UAV
    DescriptorTable,     // Table of descriptors
    Constants32Bit,      // Inline 32-bit constants
};

// Root parameter descriptor
struct RootParameter {
    RootParamType type;
    uint32_t shaderRegister;
    uint32_t registerSpace = 0;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;

    // For descriptor tables
    uint32_t numDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE_TYPE rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

    // For 32-bit constants
    uint32_t num32BitValues = 0;
};

// Static sampler descriptor
struct StaticSamplerDesc {
    uint32_t shaderRegister;
    uint32_t registerSpace = 0;
    D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL;
};

// Graphics pipeline configuration
struct GraphicsPipelineDesc {
    // Shaders
    CompiledShader vertexShader;
    CompiledShader pixelShader;

    // Root signature parameters
    std::vector<RootParameter> rootParams;
    std::vector<StaticSamplerDesc> staticSamplers;

    // Input layout (empty for fullscreen triangle)
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    // Render target formats
    std::vector<DXGI_FORMAT> rtvFormats;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;

    // Pipeline state
    D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_NONE;
    D3D12_FILL_MODE fillMode = D3D12_FILL_MODE_SOLID;
    bool depthEnable = false;
    bool blendEnable = false;

    // Multi-sampling
    UINT sampleCount = 1;

    // Debug name
    std::string debugName;
};

// Graphics pipeline wrapper
class DX12GraphicsPipeline {
public:
    DX12GraphicsPipeline() = default;
    ~DX12GraphicsPipeline() = default;

    // Non-copyable
    DX12GraphicsPipeline(const DX12GraphicsPipeline&) = delete;
    DX12GraphicsPipeline& operator=(const DX12GraphicsPipeline&) = delete;

    // Movable
    DX12GraphicsPipeline(DX12GraphicsPipeline&& other) noexcept;
    DX12GraphicsPipeline& operator=(DX12GraphicsPipeline&& other) noexcept;

    Result<void> Initialize(ID3D12Device* device, const GraphicsPipelineDesc& desc);
    void Shutdown();

    // Bind pipeline to command list
    // CRITICAL: Always set topology after binding - "Amnesia" crash prevention
    void Bind(ID3D12GraphicsCommandList* cmdList);

    // Accessors
    ID3D12PipelineState* GetPSO() const { return m_pso.Get(); }
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
    D3D12_PRIMITIVE_TOPOLOGY GetTopology() const { return m_topology; }

private:
    Result<void> CreateRootSignature(ID3D12Device* device, const GraphicsPipelineDesc& desc);
    Result<void> CreatePSO(ID3D12Device* device, const GraphicsPipelineDesc& desc);

    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    D3D12_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

// NOTE: DX12ComputePipeline is defined in DX12ComputePipeline.h with full implementation
// including SetRootConstantBufferView, SetRootDescriptorTable, SetRoot32BitConstants, etc.

} // namespace VENPOD::Graphics
