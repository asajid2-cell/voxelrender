#include "DX12ComputePipeline.h"
#include "d3dx12.h"  // Local helper for basic types
#include <spdlog/spdlog.h>

namespace VENPOD::Graphics {

Result<void> DX12ComputePipeline::Initialize(ID3D12Device* device, const ComputePipelineDesc& desc) {
    if (!device) {
        return Error("Device is null");
    }

    if (!desc.computeShader.IsValid()) {
        return Error("Compute shader is invalid");
    }

    m_debugName = desc.debugName;

    // Create root signature
    auto result = CreateRootSignature(device, desc);
    if (!result) {
        return Error("Failed to create root signature: {}", result.error());
    }

    // Create pipeline state
    result = CreatePipelineState(device, desc);
    if (!result) {
        return Error("Failed to create pipeline state: {}", result.error());
    }

    spdlog::info("DX12ComputePipeline created: {}", m_debugName);
    return {};
}

void DX12ComputePipeline::Shutdown() {
    m_pipelineState.Reset();
    m_rootSignature.Reset();
}

void DX12ComputePipeline::Bind(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList || !m_pipelineState || !m_rootSignature) return;

    cmdList->SetComputeRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pipelineState.Get());
}

void DX12ComputePipeline::SetRootConstantBufferView(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t rootIndex,
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress)
{
    if (cmdList) {
        cmdList->SetComputeRootConstantBufferView(rootIndex, gpuAddress);
    }
}

void DX12ComputePipeline::SetRootDescriptorTable(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t rootIndex,
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    if (cmdList) {
        cmdList->SetComputeRootDescriptorTable(rootIndex, gpuHandle);
    }
}

void DX12ComputePipeline::SetRoot32BitConstants(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t rootIndex,
    uint32_t num32BitValues,
    const void* data,
    uint32_t destOffset)
{
    if (cmdList && data) {
        cmdList->SetComputeRoot32BitConstants(rootIndex, num32BitValues, data, destOffset);
    }
}

void DX12ComputePipeline::Dispatch(
    ID3D12GraphicsCommandList* cmdList,
    uint32_t threadGroupCountX,
    uint32_t threadGroupCountY,
    uint32_t threadGroupCountZ)
{
    if (cmdList) {
        cmdList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
    }
}

Result<void> DX12ComputePipeline::CreateRootSignature(ID3D12Device* device, const ComputePipelineDesc& desc) {
    // Build raw D3D12 root parameters
    std::vector<D3D12_ROOT_PARAMETER1> rootParameters;
    std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRanges;

    // Reserve space to avoid reallocation invalidating pointers
    descriptorRanges.reserve(desc.rootParams.size());

    for (const auto& param : desc.rootParams) {
        D3D12_ROOT_PARAMETER1 rootParam = {};
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // Compute always ALL

        switch (param.type) {
            case RootParamType::ConstantBuffer:
                rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                rootParam.Descriptor.ShaderRegister = param.shaderRegister;
                rootParam.Descriptor.RegisterSpace = param.registerSpace;
                rootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                break;

            case RootParamType::ShaderResource:
                rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                rootParam.Descriptor.ShaderRegister = param.shaderRegister;
                rootParam.Descriptor.RegisterSpace = param.registerSpace;
                rootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                break;

            case RootParamType::UnorderedAccess:
                rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                rootParam.Descriptor.ShaderRegister = param.shaderRegister;
                rootParam.Descriptor.RegisterSpace = param.registerSpace;
                rootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
                break;

            case RootParamType::DescriptorTable: {
                D3D12_DESCRIPTOR_RANGE1 range = {};
                range.RangeType = param.rangeType;
                range.NumDescriptors = param.numDescriptors;
                range.BaseShaderRegister = param.shaderRegister;
                range.RegisterSpace = param.registerSpace;
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                descriptorRanges.push_back(range);

                rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParam.DescriptorTable.NumDescriptorRanges = 1;
                rootParam.DescriptorTable.pDescriptorRanges = &descriptorRanges.back();
                break;
            }

            case RootParamType::Constants32Bit:
                rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                rootParam.Constants.ShaderRegister = param.shaderRegister;
                rootParam.Constants.RegisterSpace = param.registerSpace;
                rootParam.Constants.Num32BitValues = param.numDescriptors;  // numDescriptors holds constant count
                break;
        }

        rootParameters.push_back(rootParam);
    }

    // Build versioned root signature desc
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSigDesc.Desc_1_1.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    if (FAILED(hr)) {
        std::string errorMsg = error ? static_cast<const char*>(error->GetBufferPointer()) : "Unknown error";
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

    // Set debug name
    if (!m_debugName.empty()) {
        std::wstring wname(m_debugName.begin(), m_debugName.end());
        wname += L"_RootSig";
        m_rootSignature->SetName(wname.c_str());
    }

    return {};
}

Result<void> DX12ComputePipeline::CreatePipelineState(ID3D12Device* device, const ComputePipelineDesc& desc) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.CS = {
        desc.computeShader.bytecode.data(),
        desc.computeShader.bytecode.size()
    };

    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        return Error("Failed to create compute pipeline state: 0x{:08X}", hr);
    }

    // Set debug name
    if (!m_debugName.empty()) {
        std::wstring wname(m_debugName.begin(), m_debugName.end());
        wname += L"_PSO";
        m_pipelineState->SetName(wname.c_str());
    }

    return {};
}

} // namespace VENPOD::Graphics
