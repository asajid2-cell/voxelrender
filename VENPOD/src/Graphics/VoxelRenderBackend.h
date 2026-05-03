#pragma once

#include <string_view>

namespace VENPOD::Graphics {

enum class VoxelRenderBackend {
    DenseLegacy,
    SparseBrick
};

VoxelRenderBackend ParseVoxelRenderBackend(std::string_view value);
VoxelRenderBackend RequestedVoxelRenderBackendFromEnvironment();
const char* ToString(VoxelRenderBackend backend);

} // namespace VENPOD::Graphics

