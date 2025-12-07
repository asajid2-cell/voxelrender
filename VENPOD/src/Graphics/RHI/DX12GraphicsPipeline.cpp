#include "DX12GraphicsPipeline.h"
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

// =============================================================================
// DX12GraphicsPipeline Implementation
// =============================================================================

DX12GraphicsPipeline::DX12GraphicsPipeline(DX12GraphicsPipeline&& other) noexcept
    : m_pso(std::move(other.m_pso))
    , m_rootSignature(std::move(other.m_rootSignature))
    , m_topology(other.m_topology)
{
    other.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

DX12GraphicsPipeline& DX12GraphicsPipeline::operator=(DX12GraphicsPipeline&& other) noexcept {
    if (this != &other) {
        m_pso = std::move(other.m_pso);
        m_rootSignature = std::move(other.m_rootSignature);
        m_topology = other.m_topology;
        other.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    return *this;
}

Result<void> DX12GraphicsPipeline::Initialize(ID3D12Device* device, const GraphicsPipelineDesc& desc) {
    if (!device) {
        return Error("DX12GraphicsPipeline::Initialize - device is null");
    }

    auto result = CreateRootSignature(device, desc);
    if (!result) {
        return result;
    }

    result = CreatePSO(device, desc);
    if (!result) {
        return result;
    }

    spdlog::info("DX12GraphicsPipeline created: {}", desc.debugName.empty() ? "unnamed" : desc.debugName);
    return {};
}

void DX12GraphicsPipeline::Shutdown() {
    m_pso.Reset();
    m_rootSignature.Reset();
}

void DX12GraphicsPipeline::Bind(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList) return;

    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pso.Get());

    // CRITICAL: Always set topology - "Amnesia" crash prevention
    // DX12 does NOT remember topology between pipeline bindings
    cmdList->IASetPrimitiveTopology(m_topology);
}

Result<void> DX12GraphicsPipeline::CreateRootSignature(ID3D12Device* device, const GraphicsPipelineDesc& desc) {
    std::vector<D3D12_ROOT_PARAMETER1> params;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;

    // Pre-allocate ranges to avoid reallocation invalidating pointers
    size_t rangeCount = 0;
    for (const auto& param : desc.rootParams) {
        if (param.type == RootParamType::DescriptorTable) {
            rangeCount++;
        }
    }
    ranges.reserve(rangeCount);

    for (const auto& param : desc.rootParams) {
        D3D12_ROOT_PARAMETER1 d3dParam = {};
        d3dParam.ShaderVisibility = param.visibility;

        switch (param.type) {
            case RootParamType::ConstantBuffer:
                d3dParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                d3dParam.Descriptor.ShaderRegister = param.shaderRegister;
                d3dParam.Descriptor.RegisterSpace = param.registerSpace;
                d3dParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                break;

            case RootParamType::ShaderResource:
                d3dParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                d3dParam.Descriptor.ShaderRegister = param.shaderRegister;
                d3dParam.Descriptor.RegisterSpace = param.registerSpace;
                d3dParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                break;

            case RootParamType::UnorderedAccess:
                d3dParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                d3dParam.Descriptor.ShaderRegister = param.shaderRegister;
                d3dParam.Descriptor.RegisterSpace = param.registerSpace;
                d3dParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
                break;

            case RootParamType::DescriptorTable: {
                D3D12_DESCRIPTOR_RANGE1 range = {};
                range.RangeType = param.rangeType;
                range.NumDescriptors = param.numDescriptors;
                range.BaseShaderRegister = param.shaderRegister;
                range.RegisterSpace = param.registerSpace;
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                ranges.push_back(range);

                d3dParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                d3dParam.DescriptorTable.NumDescriptorRanges = 1;
                d3dParam.DescriptorTable.pDescriptorRanges = &ranges.back();
                break;
            }

            case RootParamType::Constants32Bit:
                d3dParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                d3dParam.Constants.ShaderRegister = param.shaderRegister;
                d3dParam.Constants.RegisterSpace = param.registerSpace;
                d3dParam.Constants.Num32BitValues = param.num32BitValues;
                break;
        }

        params.push_back(d3dParam);
    }

    // Static samplers
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    for (const auto& sampler : desc.staticSamplers) {
        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = sampler.filter;
        samplerDesc.AddressU = sampler.addressU;
        samplerDesc.AddressV = sampler.addressV;
        samplerDesc.AddressW = sampler.addressW;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 16;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderRegister = sampler.shaderRegister;
        samplerDesc.RegisterSpace = sampler.registerSpace;
        samplerDesc.ShaderVisibility = sampler.visibility;
        staticSamplers.push_back(samplerDesc);
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
    rootSigDesc.Desc_1_1.pParameters = params.data();
    rootSigDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rootSigDesc.Desc_1_1.pStaticSamplers = staticSamplers.data();
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);

    if (FAILED(hr)) {
        std::string errorMsg = "Unknown error";
        if (error) {
            errorMsg = static_cast<const char*>(error->GetBufferPointer());
        }
        return Error("Failed to serialize root signature: {}", errorMsg);
    }

    hr = device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)
    );

    if (FAILED(hr)) {
        return Error("Failed to create root signature: 0x{:08X}", hr);
    }

    return {};
}

Result<void> DX12GraphicsPipeline::CreatePSO(ID3D12Device* device, const GraphicsPipelineDesc& desc) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

    // Root signature
    psoDesc.pRootSignature = m_rootSignature.Get();

    // Shaders
    if (desc.vertexShader.IsValid()) {
        psoDesc.VS = desc.vertexShader.GetBytecode();
    }
    if (desc.pixelShader.IsValid()) {
        psoDesc.PS = desc.pixelShader.GetBytecode();
    }

    // Input layout
    psoDesc.InputLayout.NumElements = static_cast<UINT>(desc.inputLayout.size());
    psoDesc.InputLayout.pInputElementDescs = desc.inputLayout.data();

    // Rasterizer state
    psoDesc.RasterizerState.FillMode = desc.fillMode;
    psoDesc.RasterizerState.CullMode = desc.cullMode;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.MultisampleEnable = desc.sampleCount > 1;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Blend state
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
        psoDesc.BlendState.RenderTarget[i].BlendEnable = desc.blendEnable;
        psoDesc.BlendState.RenderTarget[i].LogicOpEnable = FALSE;
        psoDesc.BlendState.RenderTarget[i].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[i].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        psoDesc.BlendState.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Depth stencil state
    psoDesc.DepthStencilState.DepthEnable = desc.depthEnable;
    psoDesc.DepthStencilState.DepthWriteMask = desc.depthEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    // Sample mask and sample count
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = desc.sampleCount;
    psoDesc.SampleDesc.Quality = 0;

    // Primitive topology
    psoDesc.PrimitiveTopologyType = desc.primitiveTopology;

    // Map topology type to actual topology
    switch (desc.primitiveTopology) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
            m_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
            m_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
            m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        default:
            m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
    }

    // Render targets
    psoDesc.NumRenderTargets = static_cast<UINT>(desc.rtvFormats.size());
    for (size_t i = 0; i < desc.rtvFormats.size() && i < 8; i++) {
        psoDesc.RTVFormats[i] = desc.rtvFormats[i];
    }
    psoDesc.DSVFormat = desc.dsvFormat;

    // Create PSO
    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr)) {
        return Error("Failed to create graphics pipeline state: 0x{:08X}", hr);
    }

    // Set debug name
    if (!desc.debugName.empty()) {
        std::wstring wideName(desc.debugName.begin(), desc.debugName.end());
        m_pso->SetName(wideName.c_str());
    }

    return {};
}

// NOTE: DX12ComputePipeline implementation is in DX12ComputePipeline.cpp

} // namespace VENPOD::Graphics
