#pragma once

// =============================================================================
// VENPOD Shader Compiler - Runtime HLSL compilation using DXC
// =============================================================================

#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include "../../Utils/Result.h"

using Microsoft::WRL::ComPtr;

namespace VENPOD::Graphics {

// Shader compilation options
struct ShaderCompileOptions {
    std::wstring entryPoint = L"main";
    std::wstring target;              // vs_6_0, ps_6_0, cs_6_0, etc.
    std::vector<std::wstring> defines;
    std::vector<std::wstring> includePaths;
    bool debugInfo = false;
    bool optimizationLevel3 = true;   // O3 by default
};

// Compiled shader bytecode
struct CompiledShader {
    std::vector<uint8_t> bytecode;
    std::string errors;
    bool success = false;

    D3D12_SHADER_BYTECODE GetBytecode() const {
        return { bytecode.data(), bytecode.size() };
    }

    bool IsValid() const { return success && !bytecode.empty(); }
};

// Shader compiler using DirectX Shader Compiler (DXC)
class ShaderCompiler {
public:
    ShaderCompiler() = default;
    ~ShaderCompiler() = default;

    // Non-copyable
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // Initialize DXC compiler
    Result<void> Initialize();
    void Shutdown();

    // Compile shader from file
    Result<CompiledShader> CompileFromFile(
        const std::filesystem::path& filePath,
        const ShaderCompileOptions& options
    );

    // Compile shader from source string
    Result<CompiledShader> CompileFromSource(
        const std::string& source,
        const std::wstring& sourceName,
        const ShaderCompileOptions& options
    );

    // Convenience methods for common shader types
    Result<CompiledShader> CompileVertexShader(
        const std::filesystem::path& filePath,
        const std::wstring& entryPoint = L"main",
        bool debug = false
    );

    Result<CompiledShader> CompilePixelShader(
        const std::filesystem::path& filePath,
        const std::wstring& entryPoint = L"main",
        bool debug = false
    );

    Result<CompiledShader> CompileComputeShader(
        const std::filesystem::path& filePath,
        const std::wstring& entryPoint = L"main",
        bool debug = false
    );

    // Set base include path for shader includes
    void SetIncludePath(const std::filesystem::path& path) { m_includePath = path; }

private:
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
    std::filesystem::path m_includePath;

    Result<CompiledShader> CompileInternal(
        const void* sourceData,
        size_t sourceSize,
        const std::wstring& sourceName,
        const ShaderCompileOptions& options
    );
};

// Custom include handler for DXC
class DxcIncludeHandler : public IDxcIncludeHandler {
public:
    DxcIncludeHandler(IDxcUtils* utils, const std::filesystem::path& basePath)
        : m_utils(utils), m_basePath(basePath), m_refCount(1) {}

    // IUnknown implementation
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDxcIncludeHandler implementation
    HRESULT STDMETHODCALLTYPE LoadSource(
        LPCWSTR pFilename,
        IDxcBlob** ppIncludeSource
    ) override;

    void AddIncludePath(const std::filesystem::path& path) {
        m_includePaths.push_back(path);
    }

private:
    IDxcUtils* m_utils;
    std::filesystem::path m_basePath;
    std::vector<std::filesystem::path> m_includePaths;
    std::atomic<ULONG> m_refCount;
};

} // namespace VENPOD::Graphics
