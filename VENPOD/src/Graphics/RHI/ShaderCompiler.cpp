#include "ShaderCompiler.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace VENPOD::Graphics {

namespace {

uint64_t AppendHash(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t AppendHash(uint64_t hash, const std::string& value) {
    return AppendHash(hash, value.data(), value.size());
}

uint64_t AppendHash(uint64_t hash, const std::wstring& value) {
    return AppendHash(hash, value.data(), value.size() * sizeof(wchar_t));
}

std::string HashToHex(uint64_t hash) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string SanitizeCacheStem(std::string value) {
    for (char& ch : value) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-';
        if (!ok) {
            ch = '_';
        }
    }
    return value;
}

std::string NarrowAscii(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (wchar_t ch : value) {
        out.push_back(ch >= 0 && ch <= 127 ? static_cast<char>(ch) : '_');
    }
    return out;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return out.empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

void WriteBinaryFileBestEffort(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open() || bytes.empty()) {
        return;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path ResolveIncludePath(
    const std::filesystem::path& includeName,
    const std::filesystem::path& sourceDir,
    const std::vector<std::wstring>& includePaths,
    const std::filesystem::path& compilerIncludePath)
{
    std::vector<std::filesystem::path> searchPaths;
    searchPaths.push_back(sourceDir);
    for (const auto& includePath : includePaths) {
        searchPaths.emplace_back(includePath);
    }
    if (!compilerIncludePath.empty()) {
        searchPaths.push_back(compilerIncludePath);
    }

    for (const auto& root : searchPaths) {
        const std::filesystem::path candidate = root / includeName;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    if (std::filesystem::exists(includeName)) {
        return includeName;
    }
    return {};
}

void HashShaderFileRecursive(
    const std::filesystem::path& path,
    const std::vector<std::wstring>& includePaths,
    const std::filesystem::path& compilerIncludePath,
    std::unordered_set<std::string>& visited,
    uint64_t& hash)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    const std::string key = ec ? path.lexically_normal().string() : canonical.string();
    if (!visited.insert(key).second) {
        return;
    }

    std::vector<uint8_t> bytes;
    if (!ReadBinaryFile(path, bytes)) {
        return;
    }

    hash = AppendHash(hash, key);
    hash = AppendHash(hash, bytes.data(), bytes.size());

    const std::string source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    static const std::regex includeRegex(R"(#\s*include\s*[<"]([^>"]+)[>"])");
    for (std::sregex_iterator it(source.begin(), source.end(), includeRegex), end; it != end; ++it) {
        const std::filesystem::path includeName((*it)[1].str());
        const std::filesystem::path resolved = ResolveIncludePath(
            includeName,
            path.parent_path(),
            includePaths,
            compilerIncludePath);
        if (!resolved.empty()) {
            HashShaderFileRecursive(resolved, includePaths, compilerIncludePath, visited, hash);
        }
    }
}

uint64_t ComputeShaderCacheHash(
    const std::filesystem::path& filePath,
    const ShaderCompileOptions& options,
    const std::filesystem::path& compilerIncludePath)
{
    uint64_t hash = 1469598103934665603ull;
    hash = AppendHash(hash, options.entryPoint);
    hash = AppendHash(hash, options.target);
    hash = AppendHash(hash, options.debugInfo ? "debug" : "nodebug");
    hash = AppendHash(hash, options.optimizationLevel3 ? "o3" : "o0");
    for (const auto& define : options.defines) {
        hash = AppendHash(hash, define);
    }
    for (const auto& includePath : options.includePaths) {
        hash = AppendHash(hash, includePath);
    }
    hash = AppendHash(hash, compilerIncludePath.wstring());

    std::unordered_set<std::string> visited;
    HashShaderFileRecursive(filePath, options.includePaths, compilerIncludePath, visited, hash);
    return hash;
}

} // namespace

// =============================================================================
// DxcIncludeHandler Implementation
// =============================================================================

HRESULT STDMETHODCALLTYPE DxcIncludeHandler::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) {
        return E_POINTER;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler)) {
        *ppvObject = static_cast<IDxcIncludeHandler*>(this);
        AddRef();
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DxcIncludeHandler::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DxcIncludeHandler::Release() {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE DxcIncludeHandler::LoadSource(
    LPCWSTR pFilename,
    IDxcBlob** ppIncludeSource)
{
    if (!pFilename || !ppIncludeSource) {
        return E_POINTER;
    }

    std::filesystem::path filename(pFilename);

    // Try to find the file in various paths
    std::filesystem::path fullPath;

    // First, try relative to base path
    if (!m_basePath.empty()) {
        fullPath = m_basePath / filename;
        if (std::filesystem::exists(fullPath)) {
            goto found;
        }
    }

    // Try each include path
    for (const auto& includePath : m_includePaths) {
        fullPath = includePath / filename;
        if (std::filesystem::exists(fullPath)) {
            goto found;
        }
    }

    // Try the filename as-is (absolute path)
    if (std::filesystem::exists(filename)) {
        fullPath = filename;
        goto found;
    }

    spdlog::error("DxcIncludeHandler: Could not find include file: {}", filename.string());
    return E_FAIL;

found:
    // Read the file
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("DxcIncludeHandler: Failed to open file: {}", fullPath.string());
        return E_FAIL;
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> contents(size);
    if (!file.read(contents.data(), size)) {
        spdlog::error("DxcIncludeHandler: Failed to read file: {}", fullPath.string());
        return E_FAIL;
    }

    // Create a blob from the file contents
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_utils->CreateBlob(
        contents.data(),
        static_cast<UINT32>(size),
        CP_UTF8,
        &sourceBlob
    );

    if (FAILED(hr)) {
        spdlog::error("DxcIncludeHandler: Failed to create blob for: {}", fullPath.string());
        return hr;
    }

    *ppIncludeSource = sourceBlob.Detach();
    return S_OK;
}

// =============================================================================
// ShaderCompiler Implementation
// =============================================================================

Result<void> ShaderCompiler::Initialize() {
    // Create DXC utils
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils));
    if (FAILED(hr)) {
        return Error("ShaderCompiler::Initialize - Failed to create DxcUtils: 0x{:08X}", hr);
    }

    // Create DXC compiler
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler));
    if (FAILED(hr)) {
        return Error("ShaderCompiler::Initialize - Failed to create DxcCompiler: 0x{:08X}", hr);
    }

    spdlog::info("ShaderCompiler initialized with DXC");
    return {};
}

void ShaderCompiler::Shutdown() {
    m_includeHandler.Reset();
    m_compiler.Reset();
    m_utils.Reset();
}

Result<CompiledShader> ShaderCompiler::CompileFromFile(
    const std::filesystem::path& filePath,
    const ShaderCompileOptions& options)
{
    if (!std::filesystem::exists(filePath)) {
        CompiledShader result;
        result.errors = "File not found: " + filePath.string();
        return Result<CompiledShader>::Ok(std::move(result));
    }

    // Read the file
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        CompiledShader result;
        result.errors = "Failed to open file: " + filePath.string();
        return Result<CompiledShader>::Ok(std::move(result));
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> contents(size);
    if (!file.read(contents.data(), size)) {
        CompiledShader result;
        result.errors = "Failed to read file: " + filePath.string();
        return Result<CompiledShader>::Ok(std::move(result));
    }

    // Set up include handler with the file's directory as base path
    std::filesystem::path basePath = filePath.parent_path();

    // Add the file's parent directory to include paths so relative includes work
    ShaderCompileOptions optionsWithBasePath = options;
    optionsWithBasePath.includePaths.push_back(basePath.wstring());

    const uint64_t cacheHash = ComputeShaderCacheHash(filePath, optionsWithBasePath, m_includePath);
    const std::string cacheStem = SanitizeCacheStem(filePath.filename().string()) + "_" +
        SanitizeCacheStem(NarrowAscii(options.target)) + "_" +
        SanitizeCacheStem(NarrowAscii(options.entryPoint)) + "_" +
        HashToHex(cacheHash);
    const std::filesystem::path cachePath =
        std::filesystem::current_path() / ".venpod_shader_cache" / (cacheStem + ".cso");

    CompiledShader cached;
    if (ReadBinaryFile(cachePath, cached.bytecode) && !cached.bytecode.empty()) {
        cached.success = true;
        spdlog::debug("Shader cache hit: {} -> {}", filePath.string(), cachePath.string());
        return Result<CompiledShader>::Ok(std::move(cached));
    }

    auto compiled = CompileInternal(
        contents.data(),
        contents.size(),
        filePath.filename().wstring(),
        optionsWithBasePath
    );
    if (compiled && compiled.value().IsValid()) {
        WriteBinaryFileBestEffort(cachePath, compiled.value().bytecode);
        spdlog::debug("Shader cache write: {} -> {}", filePath.string(), cachePath.string());
    }
    return compiled;
}

Result<CompiledShader> ShaderCompiler::CompileFromSource(
    const std::string& source,
    const std::wstring& sourceName,
    const ShaderCompileOptions& options)
{
    return CompileInternal(
        source.data(),
        source.size(),
        sourceName,
        options
    );
}

Result<CompiledShader> ShaderCompiler::CompileInternal(
    const void* sourceData,
    size_t sourceSize,
    const std::wstring& sourceName,
    const ShaderCompileOptions& options)
{
    CompiledShader result;

    if (!m_compiler || !m_utils) {
        result.errors = "Shader compiler not initialized";
        return Result<CompiledShader>::Ok(std::move(result));
    }

    // Create source blob
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_utils->CreateBlob(
        sourceData,
        static_cast<UINT32>(sourceSize),
        CP_UTF8,
        &sourceBlob
    );

    if (FAILED(hr)) {
        result.errors = "Failed to create source blob";
        return Result<CompiledShader>::Ok(std::move(result));
    }

    // Build arguments
    std::vector<LPCWSTR> arguments;

    // Entry point
    arguments.push_back(L"-E");
    arguments.push_back(options.entryPoint.c_str());

    // Target profile
    arguments.push_back(L"-T");
    arguments.push_back(options.target.c_str());

    // Debug info
    if (options.debugInfo) {
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
    }

    // Optimization
    if (options.optimizationLevel3) {
        arguments.push_back(L"-O3");
    } else {
        arguments.push_back(L"-O0");
    }

    // Note: -enable-16bit-types requires SM 6.2+, only add if needed
    // arguments.push_back(L"-enable-16bit-types");

    // Defines
    std::vector<std::wstring> defineArgs;
    for (const auto& define : options.defines) {
        defineArgs.push_back(L"-D");
        defineArgs.push_back(define);
    }
    for (const auto& arg : defineArgs) {
        arguments.push_back(arg.c_str());
    }

    // Include paths
    std::vector<std::wstring> includeArgs;
    for (const auto& path : options.includePaths) {
        includeArgs.push_back(L"-I");
        includeArgs.push_back(path);
    }
    if (!m_includePath.empty()) {
        includeArgs.push_back(L"-I");
        includeArgs.push_back(m_includePath.wstring());
    }
    for (const auto& arg : includeArgs) {
        arguments.push_back(arg.c_str());
    }

    // Create include handler
    DxcIncludeHandler* includeHandler = new DxcIncludeHandler(m_utils.Get(), m_includePath);
    for (const auto& path : options.includePaths) {
        includeHandler->AddIncludePath(path);
    }

    // Compile
    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = CP_UTF8;

    ComPtr<IDxcResult> compileResult;
    hr = m_compiler->Compile(
        &sourceBuffer,
        arguments.data(),
        static_cast<UINT32>(arguments.size()),
        includeHandler,
        IID_PPV_ARGS(&compileResult)
    );

    includeHandler->Release();

    if (FAILED(hr)) {
        result.errors = "Compile call failed";
        return Result<CompiledShader>::Ok(std::move(result));
    }

    // Check for errors
    ComPtr<IDxcBlobUtf8> errors;
    compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        result.errors = errors->GetStringPointer();
    }

    // Get compilation status
    HRESULT status;
    compileResult->GetStatus(&status);
    result.success = SUCCEEDED(status);

    if (result.success) {
        // Get the compiled bytecode
        ComPtr<IDxcBlob> shaderBlob;
        compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);

        if (shaderBlob) {
            result.bytecode.resize(shaderBlob->GetBufferSize());
            memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
        }

        spdlog::debug("Shader compiled successfully: {}", NarrowAscii(sourceName));
    } else {
        spdlog::error("Shader compilation failed: {}\n{}", NarrowAscii(sourceName), result.errors);
    }

    return Result<CompiledShader>::Ok(std::move(result));
}

Result<CompiledShader> ShaderCompiler::CompileVertexShader(
    const std::filesystem::path& filePath,
    const std::wstring& entryPoint,
    bool debug)
{
    ShaderCompileOptions options;
    options.entryPoint = entryPoint;
    options.target = L"vs_6_0";
    options.debugInfo = debug;
    options.optimizationLevel3 = !debug;

    return CompileFromFile(filePath, options);
}

Result<CompiledShader> ShaderCompiler::CompilePixelShader(
    const std::filesystem::path& filePath,
    const std::wstring& entryPoint,
    bool debug)
{
    ShaderCompileOptions options;
    options.entryPoint = entryPoint;
    options.target = L"ps_6_0";
    options.debugInfo = debug;
    options.optimizationLevel3 = !debug;

    return CompileFromFile(filePath, options);
}

Result<CompiledShader> ShaderCompiler::CompileComputeShader(
    const std::filesystem::path& filePath,
    const std::wstring& entryPoint,
    bool debug)
{
    ShaderCompileOptions options;
    options.entryPoint = entryPoint;
    options.target = L"cs_6_0";
    options.debugInfo = debug;
    options.optimizationLevel3 = !debug;

    return CompileFromFile(filePath, options);
}

} // namespace VENPOD::Graphics
