# Build And Run VENPOD

This tutorial gets the tech demo running from a clean checkout.

## 1. Install Prerequisites

Use a Windows machine with DirectX 12 support.

Install:

- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20 or newer
- Git
- PowerShell
- vcpkg

The setup script can install or locate Ninja and install vcpkg dependencies. If
you prefer to manage tools yourself, use the manual CMake path below.

## 2. Recommended Setup

From the repository root:

```powershell
cd VENPOD
.\setup.ps1
```

The script:

- imports the Visual Studio build environment
- installs or locates Ninja
- installs vcpkg packages
- checks `vendor/imgui`
- configures CMake
- builds `VENPOD.exe`

## 3. Manual Build

Open a Visual Studio Developer PowerShell or Developer Command Prompt, then run:

```powershell
cd VENPOD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

## 4. Run The Demo

```powershell
.\rebrun.ps1
```

`rebrun.ps1` rebuilds when needed and launches the current sparse
surface-authoritative sandbox path. Choose `Sandbox Mode` in the launcher.

To run the old dense renderer for comparison:

```powershell
.\rebrun.ps1 -DenseLegacy
```

To persist sparse brush edits across runs:

```powershell
.\rebrun.ps1 -SparseEditFile saves\review-edits.vsed
```

## 5. Rebuild After Edits

```powershell
.\build.ps1 -Config Release
```

For the shortest local sparse test loop:

```powershell
.\rebrun.ps1
```

Useful flags:

```powershell
.\rebrun.ps1 -Diagnostics
.\rebrun.ps1 -DisablePhysics
.\rebrun.ps1 -D3DDebug
```

For the full sparse regression gate:

```powershell
.\sparse_regression.ps1 -Config Release
```

## Expected Result

The sandbox opens in a first-person view above generated vertical voxel terrain.
The sparse path should show editable near terrain, stable mid/far continuity,
and diagnostics for sparse residency, page-table publication, ownership
counters, brush feedback, physics, and far SVO state.
