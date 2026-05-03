#include "VoxelRenderBackend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace VENPOD::Graphics {

VoxelRenderBackend ParseVoxelRenderBackend(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "sparse" ||
        normalized == "sparsebrick" ||
        normalized == "sparse-brick" ||
        normalized == "brick") {
        return VoxelRenderBackend::SparseBrick;
    }

    return VoxelRenderBackend::DenseLegacy;
}

VoxelRenderBackend RequestedVoxelRenderBackendFromEnvironment() {
    const char* backend = std::getenv("VENPOD_RENDER_BACKEND");
    if (!backend || backend[0] == '\0') {
        return VoxelRenderBackend::DenseLegacy;
    }
    return ParseVoxelRenderBackend(backend);
}

const char* ToString(VoxelRenderBackend backend) {
    switch (backend) {
        case VoxelRenderBackend::SparseBrick:
            return "sparse-brick";
        case VoxelRenderBackend::DenseLegacy:
        default:
            return "dense-legacy";
    }
}

} // namespace VENPOD::Graphics
