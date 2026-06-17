cmake_minimum_required(VERSION 3.20)
project(VENPOD VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# VENPOD - DirectX 12 voxel engine tech demo
# Designed around large GPU-resident chunked voxel worlds
# =============================================================================

# Modern C++20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vcpkg toolchain (automatically set by vcpkg or via -DCMAKE_TOOLCHAIN_FILE)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Dependencies (via vcpkg)
# =============================================================================
find_package(SDL3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# =============================================================================
# Source Files
# =============================================================================
set(VENPOD_SOURCES
    src/main.cpp
    src/main_launcher.cpp
    src/main_sandsim.cpp
    src/launcher.cpp

    # Core
    src/Core/Engine.cpp
    src/Core/Window.cpp
    src/Core/Timer.cpp
    src/Core/ServiceLocator.cpp

    # Graphics/RHI
    src/Graphics/RHI/DX12Device.cpp
    src/Graphics/RHI/DX12CommandQueue.cpp
    src/Graphics/RHI/DX12ComputePipeline.cpp
    src/Graphics/RHI/DX12GraphicsPipeline.cpp
    src/Graphics/RHI/DescriptorHeap.cpp
    src/Graphics/RHI/GPUBuffer.cpp
    src/Graphics/RHI/GPUTexture.cpp
    src/Graphics/RHI/ShaderCompiler.cpp

    # Graphics
    src/Graphics/FarVoxelOctree.cpp
    src/Graphics/BackbufferCapture.cpp
    src/Graphics/MidVoxelGpuGenPoc.cpp
    src/Graphics/MidVoxelGpuGenerator.cpp
    src/Graphics/Renderer.cpp
    src/Graphics/SparseSurfaceGpuResources.cpp
    src/Graphics/SparseVoxelGpuResources.cpp
    src/Graphics/VoxelRenderBackend.cpp
    src/Graphics/VoxelRenderer.cpp

    # Simulation
    src/Simulation/VoxelWorld.cpp
    src/Simulation/ChunkManager.cpp
    src/Simulation/PhysicsDispatcher.cpp
    src/Simulation/Chunk.cpp
    src/Simulation/InfiniteChunkManager.cpp
    src/Simulation/ChunkGenerationTest.cpp
    src/Simulation/ChunkStressTest.cpp
    src/Simulation/SparseBrickPool.cpp
    src/Simulation/SparseBrickRequestPlanner.cpp
    src/Simulation/SparseCharacterController.cpp
    src/Simulation/SparseClipmap.cpp
    src/Simulation/SparseCollision.cpp
    src/Simulation/SparseEditStore.cpp
    src/Simulation/SparsePagePublishQueue.cpp
    src/Simulation/SparsePageTable.cpp
    src/Simulation/SparseRuntimeBudget.cpp
    src/Simulation/SparseSurfaceCache.cpp
    src/Simulation/SparseSurfaceExtractor.cpp
    src/Simulation/SparseSurfaceRangeAllocator.cpp
    src/Simulation/SparseTerrainGenerator.cpp
    src/Simulation/SparseVoxelTypes.cpp
    src/Simulation/SparseVoxelWorld.cpp
    src/Simulation/HeightAtAttribution.cpp

    # Input
    src/Input/InputManager.cpp
    src/Input/BrushController.cpp

    # UI
    src/UI/ImGuiBackend.cpp
    src/UI/DebugOverlay.cpp
    src/UI/MaterialPalette.cpp
    src/UI/BrushPanel.cpp
    src/UI/PauseMenu.cpp

    # Utils
    src/Utils/FileUtils.cpp
)

set(VENPOD_HEADERS
    src/launcher.h

    # Core
    src/Core/Engine.h
    src/Core/Window.h
    src/Core/Timer.h
    src/Core/ServiceLocator.h

    # Graphics/RHI
    src/Graphics/RHI/DX12Device.h
    src/Graphics/RHI/DX12CommandQueue.h
    src/Graphics/RHI/DX12ComputePipeline.h
    src/Graphics/RHI/DX12GraphicsPipeline.h
    src/Graphics/RHI/DescriptorHeap.h
    src/Graphics/RHI/GPUBuffer.h
    src/Graphics/RHI/GPUTexture.h
    src/Graphics/RHI/ShaderCompiler.h
    src/Graphics/RHI/d3dx12.h

    # Graphics
    src/Graphics/FarVoxelOctree.h
    src/Graphics/BackbufferCapture.h
    src/Graphics/MidVoxelGpuGenPoc.h
    src/Graphics/Renderer.h
    src/Graphics/SparseSurfaceGpuResources.h
    src/Graphics/SparseVoxelGpuResources.h
    src/Graphics/VoxelRenderBackend.h
    src/Graphics/VoxelRenderer.h

    # Simulation
    src/Simulation/VoxelWorld.h
    src/Simulation/ChunkManager.h
    src/Simulation/PhysicsDispatcher.h
    src/Simulation/ChunkCoord.h
    src/Simulation/Chunk.h
    src/Simulation/InfiniteChunkManager.h
    src/Simulation/ChunkGenerationTest.h
    src/Simulation/ChunkStressTest.h
    src/Simulation/SparseBrickPool.h
    src/Simulation/SparseBrickRequestPlanner.h
    src/Simulation/SparseCharacterController.h
    src/Simulation/SparseClipmap.h
    src/Simulation/SparseCollision.h
    src/Simulation/SparseEditStore.h
    src/Simulation/SparsePagePublishQueue.h
    src/Simulation/SparsePageTable.h
    src/Simulation/SparseRuntimeBudget.h
    src/Simulation/SparseSurfaceCache.h
    src/Simulation/SparseSurfaceExtractor.h
    src/Simulation/SparseSurfaceRangeAllocator.h
    src/Simulation/SparseTerrainGenerator.h
    src/Simulation/SparseVoxelTypes.h
    src/Simulation/SparseVoxelWorld.h

    # Input
    src/Input/InputManager.h
    src/Input/BrushController.h

    # UI
    src/UI/ImGuiBackend.h
    src/UI/DebugOverlay.h
    src/UI/MaterialPalette.h
    src/UI/BrushPanel.h
    src/UI/PauseMenu.h

    # Utils
    src/Utils/Result.h
    src/Utils/FileUtils.h
    src/Utils/MortonCode.h
    src/Utils/BitPacking.h
)

# =============================================================================
# ImGui Sources (will be populated when imgui is added)
# =============================================================================
set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/backends/imgui_impl_sdl3.cpp
    vendor/imgui/backends/imgui_impl_dx12.cpp
)

# =============================================================================
# Executable
# =============================================================================
add_executable(VENPOD
    ${VENPOD_SOURCES}
    ${VENPOD_HEADERS}
    ${IMGUI_SOURCES}
)

# Include directories
target_include_directories(VENPOD PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
)

# =============================================================================
# Link Libraries
# =============================================================================
target_link_libraries(VENPOD PRIVATE
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Microsoft::DirectX-Headers
    Microsoft::DirectX-Guids
    glm::glm

    # DirectX 12 system libraries
    d3d12.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    dxcompiler.lib  # DXC shader compiler

    # Windows system libraries
    user32.lib
    gdi32.lib
    dbghelp.lib  # in-process sampling profiler (StackWalk/SymFromAddr)
    winmm.lib    # timeBeginPeriod for tight (~1ms) sampling cadence
)

# =============================================================================
# Compiler Options
# =============================================================================
if(MSVC)
    target_compile_options(VENPOD PRIVATE
        /W4         # Warning level 4
        /WX-        # Don't treat warnings as errors (for now)
        /permissive-  # Strict conformance mode
        /Zc:__cplusplus  # Correct __cplusplus macro
        /Z7         # Embed debug info in .obj (no shared-PDB contention under Ninja);
                    # keeps /O2 -> optimized build WITH symbols for the sampling profiler.
    )
    target_compile_definitions(VENPOD PRIVATE
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    # Use Windows subsystem for WinMain entry point.
    # /DEBUG emits VENPOD.pdb (symbolization); /OPT:REF,ICF restore the size/opt the
    # linker disables under /DEBUG so the profiled build still matches Release behavior.
    set_target_properties(VENPOD PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /DEBUG /OPT:REF /OPT:ICF"
    )
endif()

# =============================================================================
# Post-Build: Copy Assets
# =============================================================================
add_custom_command(TARGET VENPOD POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:VENPOD>/assets
    COMMENT "Copying assets to output directory..."
)

# Post-Build: Copy DXC DLLs for runtime shader compilation
# =============================================================================
# Find Windows SDK path for DXC DLLs
set(WIN_SDK_BIN "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
if(EXISTS "${WIN_SDK_BIN}/dxcompiler.dll")
    add_custom_command(TARGET VENPOD POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIN_SDK_BIN}/dxcompiler.dll" $<TARGET_FILE_DIR:VENPOD>/dxcompiler.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIN_SDK_BIN}/dxil.dll" $<TARGET_FILE_DIR:VENPOD>/dxil.dll
        COMMENT "Copying DXC DLLs for runtime shader compilation..."
    )
endif()

# =============================================================================
# IDE Source Grouping (for Visual Studio)
# =============================================================================
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Files" FILES ${VENPOD_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Header Files" FILES ${VENPOD_HEADERS})

# =============================================================================
# Tests
# =============================================================================
enable_testing()

add_executable(VENPODTests
    test/test_sparse_core.cpp
    src/Graphics/VoxelRenderBackend.cpp
    src/Simulation/SparseBrickPool.cpp
    src/Simulation/SparseBrickRequestPlanner.cpp
    src/Simulation/SparseCharacterController.cpp
    src/Simulation/SparseClipmap.cpp
    src/Simulation/SparseCollision.cpp
    src/Simulation/SparseEditStore.cpp
    src/Simulation/SparsePagePublishQueue.cpp
    src/Simulation/SparsePageTable.cpp
    src/Simulation/SparseRuntimeBudget.cpp
    src/Simulation/SparseSurfaceCache.cpp
    src/Simulation/SparseSurfaceExtractor.cpp
    src/Simulation/SparseSurfaceRangeAllocator.cpp
    src/Simulation/SparseTerrainGenerator.cpp
    src/Simulation/SparseVoxelTypes.cpp
    src/Simulation/SparseVoxelWorld.cpp
    src/Simulation/HeightAtAttribution.cpp
)

target_include_directories(VENPODTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(VENPODTests PRIVATE
    spdlog::spdlog
    Microsoft::DirectX-Headers
    Microsoft::DirectX-Guids
)

if(MSVC)
    target_compile_options(VENPODTests PRIVATE /W4 /WX- /permissive- /Zc:__cplusplus)
    target_compile_definitions(VENPODTests PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

add_test(NAME VENPODSparseCore COMMAND VENPODTests)
