#include "Graphics/RHI/ShaderCompiler.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path shaderRoot = "assets/shaders";
    std::filesystem::path shaderPath;
    std::wstring entry = L"main";
    std::wstring target = L"cs_6_0";
    std::vector<std::wstring> defines;
    bool debug = false;
    bool optimize = true;
};

std::wstring WidenAscii(const std::string& value) {
    std::wstring out;
    out.reserve(value.size());
    for (const char ch : value) {
        out.push_back(static_cast<unsigned char>(ch));
    }
    return out;
}

std::string NarrowAscii(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (const wchar_t ch : value) {
        out.push_back(ch >= 0 && ch <= 127 ? static_cast<char>(ch) : '_');
    }
    return out;
}

void PrintUsage() {
    std::cerr
        << "usage: shader_compile_smoke --shader <path> [--shader-root <dir>] "
        << "[--entry <name>] [--target <profile>] [--define <NAME[=VALUE]>] "
        << "[--debug] [--no-opt]\n";
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--shader") {
            if (const char* value = needValue("--shader")) {
                options.shaderPath = value;
            } else {
                return false;
            }
        } else if (arg == "--shader-root") {
            if (const char* value = needValue("--shader-root")) {
                options.shaderRoot = value;
            } else {
                return false;
            }
        } else if (arg == "--entry") {
            if (const char* value = needValue("--entry")) {
                options.entry = WidenAscii(value);
            } else {
                return false;
            }
        } else if (arg == "--target") {
            if (const char* value = needValue("--target")) {
                options.target = WidenAscii(value);
            } else {
                return false;
            }
        } else if (arg == "--define") {
            if (const char* value = needValue("--define")) {
                options.defines.push_back(WidenAscii(value));
            } else {
                return false;
            }
        } else if (arg == "--debug") {
            options.debug = true;
            options.optimize = false;
        } else if (arg == "--no-opt") {
            options.optimize = false;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }

    if (options.shaderPath.empty()) {
        std::cerr << "--shader is required\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        return 2;
    }

    VENPOD::Graphics::ShaderCompiler compiler;
    auto init = compiler.Initialize();
    if (!init) {
        std::cerr << "SHADER_COMPILE_SMOKE ok=false phase=initialize error=\""
                  << init.error() << "\"\n";
        return 3;
    }
    compiler.SetIncludePath(options.shaderRoot);

    VENPOD::Graphics::ShaderCompileOptions compileOptions;
    compileOptions.entryPoint = options.entry;
    compileOptions.target = options.target;
    compileOptions.defines = options.defines;
    compileOptions.debugInfo = options.debug;
    compileOptions.optimizationLevel3 = options.optimize;

    auto compiled = compiler.CompileFromFile(options.shaderPath, compileOptions);
    if (!compiled) {
        std::cerr << "SHADER_COMPILE_SMOKE ok=false phase=compile-call error=\""
                  << compiled.error() << "\"\n";
        compiler.Shutdown();
        return 4;
    }

    const auto& shader = compiled.value();
    if (!shader.IsValid()) {
        std::cerr << "SHADER_COMPILE_SMOKE ok=false shader=\""
                  << options.shaderPath.string() << "\" target=\""
                  << NarrowAscii(options.target) << "\" entry=\""
                  << NarrowAscii(options.entry) << "\" errors=\""
                  << shader.errors << "\"\n";
        compiler.Shutdown();
        return 5;
    }

    std::cout << "SHADER_COMPILE_SMOKE ok=true shader=\""
              << options.shaderPath.string() << "\" target=\""
              << NarrowAscii(options.target) << "\" entry=\""
              << NarrowAscii(options.entry) << "\" bytes="
              << shader.bytecode.size() << "\n";
    compiler.Shutdown();
    return 0;
}
