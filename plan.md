# Project Granular: High-Performance Voxel Physics Engine (VENPOD) (Master Plan)

**Filename:** `PROJECT_GRANULAR_PLAN.md`
**Tech Stack:** C++20 (Host), DirectX 12 (Compute), HLSL (Shaders), ImGui (UI)
**Architecture:** Data-Oriented Design (DOD), Indirect Dispatch, Sparse Voxel Grid
**Build System:** CMake + Ninja + vcpkg + PowerShell
**Target:** >100 Million Active Voxels @ 60 FPS

---

## Implementation Status

| Phase | Status | Files Created |
|-------|--------|---------------|
| 1A: Skeleton | ✅ COMPLETE | CMakeLists.txt, vcpkg.json, setup.ps1, build.ps1, run.ps1, clean.ps1, main.cpp, DX12Device, DX12CommandQueue, Window |
| 1B: Buffers | ✅ COMPLETE | DescriptorHeap.h/.cpp, GPUBuffer.h/.cpp, ShaderCompiler.h/.cpp, d3dx12.h, BitPacking.h |
| 1C: Rendering | ✅ COMPLETE | DX12GraphicsPipeline.h/.cpp, DX12ComputePipeline, Renderer.h/.cpp, VoxelRenderer.h/.cpp, VS_Fullscreen.hlsl, PS_Raymarch.hlsl, SharedTypes.hlsli |
| 2A: Compute | 🔲 Pending | DX12ComputePipeline, VoxelWorld, PhysicsDispatcher |
| 2B: Physics | 🔲 Pending | CS_Gravity.hlsl, MortonCode.hlsli |
| 2C: Raymarcher | 🔲 Pending | PS_Raymarch.hlsl (full DDA) |
| 3A: Chunks | 🔲 Pending | ChunkManager, CS_ChunkScanner.hlsl |
| 3B: Input | 🔲 Pending | InputManager, BrushController, CS_Brush.hlsl |
| 4A: ImGui | 🔲 Pending | ImGuiBackend, DebugOverlay, MaterialPalette |
| 4B: Fluids | 🔲 Pending | CS_Liquids.hlsl |
| 5: IO | 🔲 Pending | FileUtils, CS_RLECompress/Decompress.hlsl |
| 6: Polish | 🔲 Pending | Engine, Timer, ServiceLocator, PS_Debug.hlsl |

---

## 🏗️ Phase 1: The Foundation (Core Architecture)
**Goal:** Initialize the GPU context, manage raw memory, and prove read/write capabilities.

### 1.1. Window & Context
- [x] **Initialize Window:** SDL3 window with HWND for DX12 swapchain ✅
- [x] **Device Context:** DX12 Device with debug layer + DRED ✅
- [x] **Command Queue:** Direct Command Queue with fence-based sync ✅
- [x] **Swap Chain:** Triple-buffered Swap Chain (FLIP_DISCARD) ✅

### 1.2. Memory Architecture (The "God Buffers")
- [x] **Define Shared Types:** Create `SharedTypes.hlsli` (C++/HLSL compatible struct definitions). ✅
- [x] **Voxel Grid Allocation:** Allocate 2x `RWStructuredBuffer<uint>` (Ping-Pong buffers). ✅
    - *Size:* 256³ default (~64MB per buffer), configurable up to 1024³.
- [x] **Palette Allocation:** Allocate `Texture1D<float4>` (256 pixels) for material colors. ✅
- [x] **Bit-Packing Macros:** Implement bitwise helpers (`Pack()`, `Unpack()`) in HLSL and C++ to isolate Material, Velocity, and State bits. ✅

### 1.3. The Renderer (Volumetric DDA)
- [x] **Screen Quad:** Implement a Vertex Shader that outputs a full-screen triangle (using `SV_VertexID`). ✅
- [ ] **Ray Generation:** Calculate Ray Origin/Dir from Camera Inverse View-Projection matrix.
- [ ] **Basic DDA:** Implement the Digital Differential Analyzer algorithm to step through the grid.
- [x] **Debug Render:** Output UV gradient to prove the pixel shader pipeline works. ✅

### 1.4. Basic Compute Pipeline
- [x] **DX12ComputePipeline:** Root signature and PSO creation for compute shaders. ✅
- [ ] **Initialization Shader:** `CS_Init`: Fill buffer with Perlin/Simplex noise.
- [ ] **Stub Physics Shader:** `CS_GravityStub`: Read Buffer A, Write `0` to Buffer B (delete matter) to test write permissions.
- [x] **Barrier Management:** Implement `ResourceBarrier` to transition buffers between `UAV` (Compute) and `SRV` (Pixel). ✅

---

## 🚀 Phase 2: The Simulation Loop (Basic Physics)
**Goal:** Implementing the Cellular Automata rules on the GPU.

### 2.1. Thread Topology & Addressing
- [ ] **Thread Group:** Set `[numthreads(8, 8, 8)]` (512 threads per group) to align with cache lines.
- [ ] **Morton Codes:** Implement `EncodeMorton3(x,y,z)` and `DecodeMorton3(index)` in HLSL to map 1D index to 3D space.
- [ ] **Boundary Checks:** Ensure threads kill themselves if `DTid` is outside world bounds to prevent GPU hangs (TDR).

### 2.2. Gravity & Collision (The Kernel)
- [ ] **Double Buffering:** Bind Buffer A (SRV) as Input, Buffer B (UAV) as Output.
- [ ] **The "Pass" System:** Implement Checkerboard rendering (Update Even coordinates frame N, Odd coordinates frame N+1) to minimize collision locks.
- [ ] **Basic Gravity:** Logic: `if (Self != Air && Below == Air) Move(Down)`.
- [ ] **Atomic Safety:** Use `InterlockedCompareExchange` on the *Target* voxel to ensure two grains don't overwrite each other.

### 2.3. Stochastic Behavior (RNG)
- [ ] **PCG Hash:** Implement a fast hash function. Input: `VoxelIndex ^ Time ^ FrameCount`.
- [ ] **Dispersion:** If `Down` is blocked, use RNG to choose `Down-Left` or `Down-Right`.

---

## ⚡ Phase 3: Optimization (The Billion Voxel Tier)
**Goal:** Smart execution. Only process what moves.

### 3.1. Chunk Management
- [ ] **Chunk Definition:** World is divided into 32³ or 64³ "Chunks".
- [ ] **Chunk State Buffer:** Allocate a buffer to track: `IsActive`, `HasMoved`, `SleepTimer` per chunk.
- [ ] **Scanner Shader:** `CS_Scan`: Runs *one thread per chunk*. Checks if chunk contains non-Air/non-Bedrock. Writes to `ActiveChunkList`.

### 3.2. Indirect Execution
- [ ] **Indirect Argument Buffer:** Create a buffer containing `{ ThreadGroupCountX, 1, 1 }`.
- [ ] **Counter Reset:** Clear the counter to 0 at start of frame.
- [ ] **DispatchIndirect:** Replace `Dispatch()` with `DispatchIndirect()`. GPU now decides its own workload size.

### 3.3. The Wake-Up Chain
- [ ] **Wake Logic:** If a particle moves from Chunk A into Chunk B, set Chunk B's state to `Active`.
- [ ] **Wave Intrinsics:** Use `WaveActiveBitOr` (HLSL) to coalesce wake-up writes (reduce VRAM contention).

---

## 💧 Phase 4: Advanced Physics (Fluids & Thermo)
**Goal:** Complex interactions and state changes.

### 4.1. Momentum & Velocity
- [ ] **Velocity Packing:** Use 8 bits:
    - 3 bits: Y Velocity (Vertical acceleration).
    - 3 bits: Speed (Lateral).
    - 2 bits: Direction (N/S/E/W).
- [ ] **Inertial Move:** Before applying Gravity, apply Lateral Velocity.

### 4.2. Hydrodynamics (Fast Water)
- [ ] **Multi-Step DDA:** Liquids attempt to move `N` steps horizontally in one frame.
- [ ] **Pressure Check:** If blocked, check diagonals.

### 4.3. Density & Buoyancy
- [ ] **Density LUT:** Hardcode densities (Oil < Water < Sand < Stone).
- [ ] **Displacement Logic:** If `Self.Density > Below.Density` AND `Below != Solid`, swap positions.

### 4.4. Thermodynamics
- [ ] **Heat Buffer:** (Optional) Parallel `R8_UINT` buffer for temperature.
- [ ] **State Changes:** `if (Mat == ICE && Temp > 32) Mat = WATER`.
- [ ] **Fire Spread:** `if (Neighbor == FIRE && Self == WOOD && RNG < Chance) Self = FIRE`.

---

## 🎨 Phase 5: Rendering & Visuals
**Goal:** Volumetric rendering logic.

### 5.1. Empty Space Skipping (Acceleration)
- [ ] **Macro-Grid Traversal:** In Raymarcher, check the `ChunkStateBuffer`.
- [ ] **Leapfrogging:** If Chunk is empty, advance Ray `step_size` by 32 units instantly.

### 5.2. Lighting & Depth
- [ ] **Normal Calculation:** Estimate normal by sampling density of 6 neighbors (`GradientVector`).
- [ ] **Directional Light:** Standard Dot Product (`N dot L`).
- [ ] **Voxel AO:** Darken pixel based on how many of the 8 surrounding voxels are occupied.

### 5.3. Material Properties
- [ ] **Emission:** If `Mat == LAVA`, skip lighting calc and output bright color + Bloom threshold.
- [ ] **Transparency:** If `Mat == GLASS/WATER`, accumulate color and continue raymarching (Refraction).

---

## 🛠️ Phase 6: Interaction & Tooling
**Goal:** The creative tools.

### 6.1. The Brush Engine
- [ ] **Input Buffer:** Map Mouse X/Y and Click State to a Constant Buffer.
- [ ] **Brush Shader:** Runs *before* physics. Raycasts from camera to world.
- [ ] **SDF Painting:** `if (distance(voxel, hit_point) < radius) Voxel = BrushMat`.

### 6.2. Advanced Tools
- [ ] **Material Masking:** "Only Paint X if Target is Y" (e.g., Paint Grass on Dirt).
- [ ] **Explosion Tool:** Radial force that sets outward velocity on all particles in radius.

### 6.3. Debugging
- [ ] **Slice View:** Discard pixels in Shader if `Voxel.z > ClipPlane`.
- [ ] **Heatmap Mode:** Render `SleepTimer` value as Red->Green color to visualize optimization.

---

## 💾 Phase 7: IO & Persistence
**Goal:** Save/Load functionality.

### 7.1. RLE Compression
- [ ] **Run-Length Encoder:** Compute Shader that linearizes the Morton Grid and compresses: `[Count, MatID], [Count, MatID]`.
- [ ] **File IO:** Write the compressed blob to `.bin` file.

### 7.2. "Blueprints" (Copy/Paste)
- [ ] **Clipboard Buffer:** Temporary buffer to store a copied selection.
- [ ] **Paste Shader:** Kernel to write Clipboard Buffer back to World at new Mouse Position.

---

## 🧠 Appendix: Data Structures

### The Voxel (32-bit `uint`)
Bit-packed integer enabling 4GB for 1024³ resolution. 

The Data Structure (The "Voxel")

To fit this on the GPU, you need to be ruthless with memory. Do not use classes or pointers. You need a "Plain Old Data" (POD) struct.

The Bit-Packing Strategy: You don't need a float for velocity. You can pack everything into a single uint32_t or two.

| Bit Range | Component | Description |
| :--- | :--- | :--- |
| **31-24** | `State` | Flags: `IsStatic`, `IsIgnited`, `HasMoved`, `Life` (4 bits) |
| **23-16** | `Velocity` | `Y_Vel` (3 bits signed), `X_Speed` (3 bits), `Heading` (2 bits) |
| **15-08** | `Variant` | Random variation for texture lookups / visual noise |
| **07-00** | `Material` | ID (0-255). 0=Air, 1=Sand, 2=Water, etc. |

### The Chunk Control (Struct)
Used for the "Sparse" optimization.
```cpp
struct ChunkControl {
    uint is_active;      // 0 or 1. If 0, Physics Shader skips this chunk.
    uint sleep_timer;    // How many frames since last movement?
    uint particle_count; // Debugging metric.
    uint padding;        // Align to 16 bytes.
};